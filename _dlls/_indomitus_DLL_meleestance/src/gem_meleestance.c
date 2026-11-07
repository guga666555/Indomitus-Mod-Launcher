/* GEM MeleeStance 0.2 - Dawn of War style melee toggle + melee charge
 * Call to Arms: Gates of Hell (GEM 2, 1.067)
 *
 * 1. Stance. While a human carries the script tag <Tag> (default "melee_stance"), the engine's ammo choice
 *    (set\target\ammo.set) only accepts ammo whose {tag} list holds one of <MeleeAmmoTags>, so the unit
 *    draws its melee weapon and keeps using it; guns, pistols and grenades are refused. Untagged = vanilla.
 *    The {ready_for_shot} limit is lifted for melee ammo of a tagged unit, so ammo.set rows that only allow
 *    melee "when already in hand" also let a tagged unit draw it from the belt.
 * 2. Charge. A human tagged <ChargeTag> (default "has_melee_weapon") whose melee weapon is in hand (or who is
 *    in stance) and who sees an enemy beyond melee reach gets the same attack order a double-click gives
 *    (Order::eAttack on that enemy), so he runs in and fights. Nearest enemy wins; one order per
 *    <ChargeCooldownMs> per unit unless the target changes.
 * A human linked to something (vehicle crew / gunner, passenger) is left alone by both.
 *
 * Engine (game exe offsets, found by pattern at runtime, same code in editor/server):
 *   exe+0x914180 MatchAmmo(ctx) -> ammo.set row or NULL, called per candidate ammo by every weapon
 *   choice site. ctx (built by exe+0x9140A0): +0 ammo eStuff*, +8 shooter entity, +0x10 target info
 *   (+0x18 target entity, +0x28 float3 position), +0x18 byte "item in hand", +0x19 charging.
 *   The enemy scan exe+0x9580BA (the only function that calls MatchAmmo twice) calls it for every seen
 *   enemy BEFORE its own range check; only calls from there feed the charge (the UI also calls MatchAmmo,
 *   locally, which must not change the simulation in MP).
 *   Range(ammo stuff) exe+0x7E0AC0 -> float, compared with |shooter+0x74 - target pos| in MatchAmmo.
 *   Attack order (as the UI's attack click, exe+0xB71374):
 *     ref[0x80]: vtable, +8 type (1 entity), +0xC pos, +0x18 entity, +0x20 Tag (volume)
 *       built as MakeRef exe+0xB74420 does: vt, TagCtor(ref+0x20) exe+0x556220, RefInit(ref) exe+0xB8D30,
 *       SetEntity(ref, ent, &tag) exe+0x83DFA0 (registers the ref on the entity); freed by RefDtor exe+0x83E4F0
 *       + TagDtor(ref+0x20) exe+0x5564E0.
 *     order = MakeAttack(&ref, 0) exe+0x8EA230 (new 0x108-byte Order::eAttack, copies the ref)
 *     eActor::AddOrder(actor, order, {int 1, uint [exe+0x1556DE8]}) exe+0x7EFA10 (frees the order on refusal)
 *   ammo {tag} Tags object at stuff+0xEC8 (read from the code), its tag vector at Tags+0x318.
 *   name Tags at stuff+0x868. Entity script tags vector at entity+0x4E8. entity byte +0x30: 2 actor, 4 human.
 *   entity dword +0x1C0 bit 28 = linked (engine: mov eax,[r+1C0h]; shr eax,1Ch; test al,1).
 *   brain = entity+0xDC8, [brain+0x398]&3 == 1 = player-controlled (MatchAmmo's enable_for_user test).
 * The MatchAmmo prologue (14 bytes: mov [rsp+20h],rbx; push rbp/rsi/rdi/r12/r13/r14) is moved to a
 * trampoline and replaced by an absolute jump to ms_hook().
 * Charge orders are queued during the scan and issued on a later AI tick (from the next scan of another
 * unit), never inside the scan that found the enemy, so the scanned unit's inventory loop is not disturbed.
 *
 * Build: build.bat      Offline finder + filter test: build.bat test
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>

#define MS_VERSION "0.6.10"
#define MAXT 24
#define TLEN 48

/* ---------------------------------------------------------------- config */
static int  cfg_enabled = 1, cfg_log = 1, cfg_skip_linked = 1, cfg_lift_ready = 1;
static char cfg_tag[TLEN] = "melee_stance", cfg_atag[TLEN] = "melee_always";
static char cfg_mtags[MAXT][TLEN] = { "melee_vs_human", "melee_vs_all", "medium_inf_melee_target",
                                      "medium_veh_melee_target", "heavy_veh_melee_target" };
static int  n_mtags = 5;
static int  cfg_charge = 1, cfg_charge_inhand = 1, cfg_charge_player = 1;
static char cfg_ctag[TLEN] = "has_melee_weapon";
static uint32_t cfg_charge_cd = 4000, cfg_retarget = 1500;
static float cfg_charge_max = 2000.0f;    /* world units (20 = 1 m), 0 = any enemy the scan sees */
static float cfg_charge_min = 60.0f;      /* world units: closer than this = already fighting, no new order */
static int   cfg_down_on = 1;             /* [Downed]: no weapon is chosen against a target carrying the downed tag */
static char  cfg_downtag[1][TLEN] = { "start_personage" };   /* 40k_human.inc start_personage / stop_personage */
static uint32_t st_down;
static int   cfg_blow_on = 1;             /* [BlowReach]: engine paired-melee start distance per blow class */
static float cfg_blow[3] = { 45.0f, 45.0f, 45.0f };   /* near / medium / far, world units (vanilla 17 / 29 / 35) */
static uint32_t cfg_enttags = 0x4E8;      /* entity script tag vector */
static uint32_t cfg_ammotags = 0xEC8;     /* stuff {tag} Tags object; replaced by the offset in the engine code */
static uint32_t cfg_namtags = 0x868;      /* stuff name Tags object */
#define TAGS_VEC 0x318                    /* vector inside a Tags object (0x868->0xB80, 0xEC8->0x11E0, 0x1D0->0x4E8) */
static uint32_t cfg_linkoff = 0x1C0, cfg_linkbit = 28;

/* ---------------------------------------------------------------- log */
static char  g_logpath[MAX_PATH];
static FILE *g_log;
static int   g_lines, g_clines, g_hlines;
static void logf_(const char *fmt, ...)
{
    if (!g_log) {
        if (!g_logpath[0]) return;
        g_log = fopen(g_logpath, "a");
        if (!g_log) return;
    }
    va_list ap; va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
}

/* ---------------------------------------------------------------- tags */
static char lc(char c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

/* MSVC std::string at p: buf/ptr +0, size +0x10, cap +0x18 */
static const char *str_get(const uint8_t *p, size_t *n)
{
    uint64_t size = *(const uint64_t *)(p + 0x10);
    uint64_t cap  = *(const uint64_t *)(p + 0x18);
    if (cap < size || cap > 0x10000 || size == 0) return NULL;
    *n = (size_t)size;
    return cap > 15 ? *(const char *const *)p : (const char *)p;
}

static int eq_ci(const char *s, size_t n, const char *z)
{
    size_t m = strlen(z);
    if (n != m) return 0;
    for (size_t i = 0; i < n; i++) if (lc(s[i]) != lc(z[i])) return 0;
    return 1;
}

/* tag vector at v (begin,end of 0x60-byte Tags, std::string at +0x40):
 * returns 1 if any tag equals one of list[0..n), 0 if none, -1 on a broken vector */
static int vec_has(const uint8_t *v, char list[][TLEN], int n)
{
    const uint8_t *b = *(const uint8_t *const *)v;
    const uint8_t *e = *(const uint8_t *const *)(v + 8);
    if (!b || !e) return 0;
    if (e < b) return -1;
    uintptr_t len = (uintptr_t)(e - b);
    if (len % 0x60 || len > 0x60 * 1024) return -1;
    for (const uint8_t *t = b; t < e; t += 0x60) {
        size_t k; const char *s = str_get(t + 0x40, &k);
        if (!s) continue;
        for (int i = 0; i < n; i++) if (eq_ci(s, k, list[i])) return 1;
    }
    return 0;
}

/* "name1 name2 ..." of a tag vector, for the log */
static void vec_str(const uint8_t *v, char *out, size_t cap)
{
    out[0] = 0;
    const uint8_t *b = *(const uint8_t *const *)v, *e = *(const uint8_t *const *)(v + 8);
    if (!b || !e || e < b || (uintptr_t)(e - b) % 0x60 || (uintptr_t)(e - b) > 0x60 * 64) return;
    for (const uint8_t *t = b; t < e; t += 0x60) {
        size_t k; const char *s = str_get(t + 0x40, &k);
        if (!s) continue;
        size_t l = strlen(out);
        if (l + k + 2 >= cap) break;
        if (l) out[l++] = ' ';
        memcpy(out + l, s, k); out[l + k] = 0;
    }
}

/* ---------------------------------------------------------------- engine calls */
typedef void *(*match_t)(uint8_t *ctx);
typedef void  (*vfn1_t)(void *);
typedef void  (*setent_t)(void *ref, void *ent, void *tag);
typedef void *(*mkatk_t)(void *ref, uint8_t flag);
typedef void *(*mkmove_t)(const float *xy, int flag);   /* Order::eMove to a ground point (game exe+0x8F3600) */
typedef void *(*addord_t)(void *actor, void *order, void *params);
typedef float (*range_t)(void *stuff);
/* 0.6.9: the player's own orders. Order::eFight (game exe+0x8E7550, (entity *target) -> order) is what the attack_melee
 * action issues (Action::e_attack_melee slot 17, game exe+0xB71F40). The UI's click move is the 0xF8-byte order from game
 * exe+0x8D2B20 ((float *xy, byte kind=1) -> order, then word order+0x18 = 4, as Action::e_move slot 17 game exe+0xB65F50
 * does), preceded by Order::eMovementChange(pose, mode, wait) (game exe+0x8F3930; mode table {0,1,1,2,3,0,1,2}: 2 = run,
 * the human battle state's mode, 3 = the fastest) for the sprint. */
typedef void *(*fight_t)(void *target);
typedef void *(*mvchange_t)(int pose, int mode, uint8_t wait);
typedef void *(*uimove_t)(const float *xy, uint8_t kind);
static match_t  g_orig;
static fight_t    g_fight;
static mvchange_t g_mvchange;
static uimove_t   g_uimove;
static int cfg_runmode = 2;              /* [Charge] RunMode: eMovementChange mode queued before the far move order (2 = run, Guga-tested), -1 = none */
static vfn1_t   g_tagctor, g_tagdtor, g_refinit, g_refdtor;
static setent_t g_setent;
static mkatk_t  g_mkatk;
static mkmove_t g_mkmove;
static addord_t g_addorder;
static range_t  g_range;
static void    *g_refvt;
static volatile uint32_t *g_ordstamp;
static uint8_t *g_scan_lo, *g_scan_hi;     /* the enemy scan (return-address window) */

/* ---------------------------------------------------------------- stance filter */
static char g_tagl[2][TLEN], g_ctagl[1][TLEN];
static int  n_tagl = 1;
static uint32_t st_reject, st_allow, st_charge_q, st_charge_go, st_charge_drop;

enum { PASS = 0, REJECT = 1, MELEE = 2 };

/* units whose weapon choice offered melee ammo recently (any scan lists every carried item, belt included);
 * the stance is only enforced on them, so a toggled unit without a melee weapon keeps shooting */
typedef struct { const void *unit; uint32_t ms; } seen_t;
#define NSEEN 512
#define SEEN_MS 20000
static seen_t g_seen[NSEEN];
static volatile uint32_t *g_clock;
static uint32_t now_ms(void) { return g_clock ? *g_clock : 0; }
static void melee_seen(const void *u)
{
    uint32_t h = (uint32_t)(((uintptr_t)u >> 4) * 2654435761u) % NSEEN, now = now_ms();
    seen_t *slot = &g_seen[h];
    for (int i = 0; i < 16; i++) {
        seen_t *s = &g_seen[(h + i) % NSEEN];
        if (s->unit == u || !s->unit || (uint32_t)(now - s->ms) > SEEN_MS) { slot = s; break; }
    }
    slot->unit = u; slot->ms = now;
}
static int carries_melee(const void *u)
{
    if (!g_clock) return 1;                                  /* no clock found: old behaviour (always enforce) */
    uint32_t h = (uint32_t)(((uintptr_t)u >> 4) * 2654435761u) % NSEEN, now = now_ms();
    for (int i = 0; i < 16; i++) {
        seen_t *s = &g_seen[(h + i) % NSEEN];
        if (s->unit == u) return (uint32_t)(now - s->ms) <= SEEN_MS;
    }
    return 0;
}

static int is_linked(const uint8_t *ent) { return cfg_skip_linked && ((*(const uint32_t *)(ent + cfg_linkoff) >> cfg_linkbit) & 1); }

/* what to do with this candidate: PASS (vanilla), REJECT (not melee, unit in stance), MELEE (melee ammo, unit in stance) */
static int ms_decide(const uint8_t *ctx)
{
    if (!cfg_enabled || !ctx) return PASS;
    const uint8_t *ammo = *(const uint8_t *const *)ctx;
    const uint8_t *ent  = *(const uint8_t *const *)(ctx + 8);
    if (!ammo || !ent) return PASS;
    if (!(ent[0x30] & 0x04)) return PASS;                                       /* humans only */
    if (vec_has(ent + cfg_enttags, g_tagl, n_tagl) != 1) return PASS;           /* not in melee stance (toggle or always tag) */
    if (is_linked(ent)) return PASS;                                            /* crew/passenger */
    int m = vec_has(ammo + cfg_ammotags + TAGS_VEC, cfg_mtags, n_mtags);
    if (m < 0) return PASS;
    if (m) { melee_seen(ent); return MELEE; }
    return carries_melee(ent) ? REJECT : PASS;              /* no melee weapon seen on him: keep his guns */
}

/* ---------------------------------------------------------------- charge */
#define REFSZ 0x100
typedef struct {
    __attribute__((aligned(16))) uint8_t uref[REFSZ];   /* weak refs: the engine clears them if the entity goes */
    __attribute__((aligned(16))) uint8_t tref[REFSZ];
    uint8_t *unit, *tgt;
    float dist;
    uint32_t clock;
    int used;
} pend_t;
#define NPEND 64
static pend_t g_pend[NPEND];

/* per unit: last charge order (ms, target as a weak ref, distance when ordered / last seen closing) and last take-item order */
typedef struct {
    const void *unit, *tgt; uint32_t ms, draw_ms, chk_ms; float lastd; int has_ref, held;   /* held: 0 unknown, 1 melee in hand, 2 not */
    int phase; float ox, oy;                 /* phase: 1 = move order running at the enemy (ox/oy = its position then), 2 = attack order */
    __attribute__((aligned(16))) uint8_t tref[REFSZ];
} last_t;
static float cfg_close = 150.0f;         /* [Charge] CloseDistance: inside it the attack order, outside a move order to the enemy */
static float cfg_refresh = 80.0f;        /* [Charge] MoveRefreshUnits: the move order is re-issued when the enemy moved this far from the ordered spot */
#define PROGRESS_SAMPLE_MS 1000          /* the stall test compares distances a second apart, not tick to tick */
#define NLAST 256
static last_t g_last[NLAST];
static float cfg_switch = 0.6f;          /* [Charge] SwitchRatio: another enemy only when closer than this x the current target's distance */
static float cfg_progress = 10.0f;       /* world units the unit must have closed on its target between checks to count as still charging */

static void ref_make(uint8_t *ref, void *ent)
{
    memset(ref, 0, REFSZ);
    *(void **)ref = g_refvt;
    g_tagctor(ref + 0x20);
    g_refinit(ref);
    __attribute__((aligned(16))) uint8_t tag[0x80];
    memset(tag, 0, sizeof tag);
    g_tagctor(tag);
    g_setent(ref, ent, tag);
    g_tagdtor(tag);
}
static void ref_free(uint8_t *ref)
{
    *(void **)ref = g_refvt;
    g_refdtor(ref);
    g_tagdtor(ref + 0x20);
}
static void *ref_ent(const uint8_t *ref) { return *(const uint32_t *)(ref + 8) == 1 ? *(void *const *)(ref + 0x18) : NULL; }

static void last_reset(last_t *l, const void *unit)
{
    if (l->has_ref) ref_free(l->tref);
    l->unit = unit; l->tgt = NULL; l->ms = 0; l->draw_ms = 0; l->chk_ms = 0; l->lastd = 0; l->has_ref = 0; l->held = 0;
    l->phase = 0; l->ox = l->oy = 0;
}
static last_t *last_of(const void *unit, int make)
{
    uint32_t h = (uint32_t)(((uintptr_t)unit >> 4) * 2654435761u) % NLAST;
    for (int i = 0; i < 16; i++) {
        last_t *l = &g_last[(h + i) % NLAST];
        if (l->unit == unit) return l;
        if (make && (!l->unit || (uint32_t)(*g_clock - l->ms) > 60000)) { last_reset(l, unit); return l; }
    }
    if (!make) return NULL;
    last_reset(&g_last[h], unit);
    return &g_last[h];
}
/* remember the enemy a charge order went to */
static void last_order(last_t *l, const void *tgt, float d, uint32_t now)
{
    if (l->has_ref) ref_free(l->tref);
    ref_make(l->tref, (void *)tgt); l->has_ref = 1;
    l->tgt = tgt; l->lastd = d; l->ms = now; l->chk_ms = now;
}
static float dist3(const uint8_t *a, const uint8_t *b)
{
    float dx = *(const float *)(a + 0x74) - *(const float *)(b + 0x74);
    float dy = *(const float *)(a + 0x78) - *(const float *)(b + 0x78);
    float dz = *(const float *)(a + 0x7C) - *(const float *)(b + 0x7C);
    return sqrtf(dx * dx + dy * dy + dz * dz);
}

/* issue the queued orders recorded on an earlier tick, except for the unit being scanned right now */
static uint8_t *g_draw_action;
static int draw_melee(uint8_t *action, void **vt, uint8_t *u);

static void charge_flush(const uint8_t *scanning)
{
    uint32_t now = *g_clock;
    for (int i = 0; i < NPEND; i++) {
        pend_t *p = &g_pend[i];
        if (!p->used || p->clock == now || p->unit == scanning) continue;
        void *u = ref_ent(p->uref), *t = ref_ent(p->tref);
        if (u == p->unit && t == p->tgt && !is_linked(p->unit)) {
            /* a direct attack order fires whatever the unit HOLDS (it bypasses the ammo.set choice the stance filters),
             * so a unit whose melee weapon is not in hand yet (no charge tag, set by skin_weapon.inc for the held
             * melee weapon) gets a plain move order to the enemy instead; near it, the stance choice draws the melee
             * weapon, the tag appears and the next charge order is the attack order (0.7.1 video: stance marines ran
             * in and fired flamers / bolt pistols) */
            /* 0.6.7 video + log: an attack order on an enemy 800 units out does not make a stance marine run at it (no
             * usable weapon at that range: guns refused, sword out of reach), the unit shuffles and stands. So the far part
             * of the charge is a plain move order to the enemy's position (units run), refreshed when the enemy moves;
             * the attack order (the double-click) is given only inside CloseDistance, where the engine fights with the sword. */
            int noweap = vec_has(p->unit + cfg_enttags, g_ctagl, 1) != 1;   /* melee weapon not in hand */
            float xy[2] = { *(const float *)(p->tgt + 0x74), *(const float *)(p->tgt + 0x78) };
            int mv = (g_uimove || g_mkmove) && cfg_close > 0 && p->dist > cfg_close;
            if (mv && (xy[0] * xy[0] + xy[1] * xy[1]) < 1.0f) mv = 0;        /* engine asserts "Moving to zero bug!" */
            void *ok;
            const char *kind;
            int phase = 0;
            if (noweap && g_draw_action) {                    /* draw the melee weapon first (take-item order); next charge = move/attack */
                int issued = draw_melee(g_draw_action, *(void ***)g_draw_action, p->unit);
                ok = (void *)1; kind = issued ? "draw" : "draw-wait"; mv = 0;   /* draw-wait: a take-item order is still running (DrawCooldownMs) */
            } else {
                uint32_t params[2] = { 1, *g_ordstamp };
                void *order;
                if (mv) {                                     /* the player's double-click: movement mode, then eMoveTask */
                    /* 0.6.9 test: the UI click-move order is Order::eAdvance, which queues its own eMovementChange(mode 1, wait)
                     * = the slow weapon-up walk, overriding any mode set before it. The double-click path of Action::e_move
                     * uses MakeMove = Order::eMoveTask (the AI's run order) instead, so that is the far charge now. */
                    if (g_mvchange && cfg_runmode >= 0) {
                        void *mc = g_mvchange(-1, cfg_runmode, 0);
                        if (mc) g_addorder(p->unit, mc, params);
                    }
                    order = g_mkmove ? g_mkmove(xy, 0) : g_uimove(xy, 1);
                    if (order) *(uint16_t *)((uint8_t *)order + 0x18) = 4;   /* as Action::e_move does on every click move */
                    kind = g_mkmove ? "run" : "advance"; phase = 1;
                } else {                                      /* the attack_melee order (eFight); attack order if the finder failed */
                    order = g_fight ? g_fight(p->tgt) : g_mkatk(p->tref, 0);
                    kind = g_fight ? "melee" : "attack"; phase = 2;
                }
                ok = order ? g_addorder(p->unit, order, params) : NULL;
            }
            last_t *l = last_of(p->unit, 1);
            last_order(l, p->tgt, p->dist, now);
            l->phase = phase;
            l->ox = xy[0]; l->oy = xy[1];
            if (ok) st_charge_go++; else st_charge_drop++;
            if (cfg_log && g_clines < 40) {
                g_clines++;
                logf_("charge: [%u ms] unit %p -> enemy %p at %.0f %s %s (orders %u, refused %u)", now, (void *)p->unit, (void *)p->tgt,
                      p->dist, kind, ok ? "ordered" : "REFUSED by AddOrder", st_charge_go, st_charge_drop);
            }
        } else st_charge_drop++;
        ref_free(p->uref); ref_free(p->tref);
        p->used = 0;
    }
}

/* a scan found melee ammo usable vs this enemy: queue an attack order if the enemy is out of reach */
static void charge_note(const uint8_t *ctx, int inhand, int stance)
{
    uint8_t *ent = *(uint8_t *const *)(ctx + 8);
    const uint8_t *ti = *(const uint8_t *const *)(ctx + 0x10);
    const uint8_t *ammo = *(const uint8_t *const *)ctx;
    if (!ent || !ti || !ammo || !(ent[0x30] & 0x04) || is_linked(ent)) return;
    uint8_t *tgt = *(uint8_t *const *)(ti + 0x18);
    if (!tgt || tgt == ent || !(tgt[0x30] & 0x02)) return;
    if (tgt[0xDA8] == ent[0xDA8]) return;                    /* same owner player: never charge a friend */
    if (cfg_down_on && vec_has(tgt + cfg_enttags, cfg_downtag, 1) == 1) return;   /* downed */
    if (!stance && vec_has(ent + cfg_enttags, g_ctagl, 1) != 1) return;
    if (!stance && cfg_charge_inhand && !inhand) return;
    if (!cfg_charge_player) {
        const uint8_t *br = *(const uint8_t *const *)(ent + 0xDC8);
        if (br && (br[0x398] & 3) == 1) return;
    }
    if (vec_has(ammo + cfg_ammotags + TAGS_VEC, cfg_mtags, n_mtags) != 1) return;
    float dx = *(const float *)(ent + 0x74) - *(const float *)(ti + 0x28);
    float dy = *(const float *)(ent + 0x78) - *(const float *)(ti + 0x2C);
    float dz = *(const float *)(ent + 0x7C) - *(const float *)(ti + 0x30);
    float d = sqrtf(dx * dx + dy * dy + dz * dz);
    {   /* log every change of "melee weapon in hand" on a charging unit (0.6.6 test: units holstered mid-charge) */
        last_t *lh = last_of(ent, 1);
        int h = vec_has(ent + cfg_enttags, g_ctagl, 1) == 1 ? 1 : 2;
        if (lh->held != h) {
            lh->held = h;
            if (cfg_log && g_hlines < 40) { g_hlines++; logf_("held: [%u ms] unit %p melee weapon %s at %.0f", *g_clock, (void *)ent, h == 1 ? "IN HAND" : "PUT AWAY", d); }
        }
    }
    float reach = g_range((void *)ammo);
    if (d <= reach * 1.1f + 1.0f || d < cfg_charge_min) return;   /* in reach / already fighting: the engine attacks on its own */
    if (cfg_charge_max > 0 && d > cfg_charge_max) return;
    uint32_t now = *g_clock;
    last_t *l = last_of(ent, 0);
    if (l && l->ms) {
        /* one order per enemy: a new AddOrder restarts the running attack order (the unit stops and re-paths), so while
         * the unit is still closing on the enemy it was sent at, nothing is re-issued. A re-order only when the unit has
         * stalled (no progress since the last check, after CooldownMs), the enemy is gone, or another enemy is much closer. */
        uint32_t age = now - l->ms;
        if (age < cfg_retarget) return;
        const uint8_t *cur = l->has_ref ? (const uint8_t *)ref_ent(l->tref) : NULL;
        if (cur && cur == tgt) {
            int renew = 0;
            if (l->phase == 1) {                                        /* running at it under a move order */
                float mx = *(const float *)(tgt + 0x74) - l->ox, my = *(const float *)(tgt + 0x78) - l->oy;
                if (d <= cfg_close) renew = 1;                          /* close enough: switch to the attack order */
                else if (mx * mx + my * my > cfg_refresh * cfg_refresh) renew = 1;   /* enemy moved: new destination */
            }
            if (!renew) {
                if (age < cfg_charge_cd) return;
                if ((uint32_t)(now - l->chk_ms) < PROGRESS_SAMPLE_MS) return;
                float prev = l->lastd; l->lastd = d; l->chk_ms = now;
                if (d < prev - cfg_progress) return;                    /* still closing: let the order run */
            }
        } else if (cur) {
            if (d > dist3(ent, cur) * cfg_switch) return;               /* current enemy still there and not much farther */
        }
    }
    pend_t *free_ = NULL;
    for (int i = 0; i < NPEND; i++) {
        pend_t *p = &g_pend[i];
        if (p->used && p->unit == ent) {
            if (d >= p->dist) return;                         /* already queued a nearer enemy */
            ref_free(p->tref); ref_make(p->tref, tgt);
            p->tgt = tgt; p->dist = d; p->clock = now;
            return;
        }
        if (!p->used && !free_) free_ = p;
    }
    if (!free_) return;
    ref_make(free_->uref, ent); ref_make(free_->tref, tgt);
    free_->unit = ent; free_->tgt = tgt; free_->dist = d; free_->clock = now; free_->used = 1;
    st_charge_q++;
}

__attribute__((used, ms_abi, noinline)) void *ms_hook(uint8_t *ctx)
{
    uint8_t *ret = (uint8_t *)__builtin_return_address(0);
    int from_scan = cfg_charge && g_scan_lo && ret >= g_scan_lo && ret < g_scan_hi;
    if (from_scan) charge_flush(*(uint8_t **)(ctx + 8));
    if (cfg_down_on) {                                        /* downed (personage) target: no weapon is usable against it */
        const uint8_t *ti = *(const uint8_t *const *)(ctx + 0x10);
        const uint8_t *tg = ti ? *(const uint8_t *const *)(ti + 0x18) : NULL;
        if (tg && (tg[0x30] & 0x04) && vec_has(tg + cfg_enttags, cfg_downtag, 1) == 1) { st_down++; return NULL; }
    }
    uint8_t ready = ctx[0x18];
    int d = ms_decide(ctx);
    void *r;
    if (d == REJECT) {
        st_reject++;
        if (cfg_log && g_lines < 40) {
            g_lines++;
            char nm[200];
            vec_str(*(uint8_t **)ctx + cfg_namtags + TAGS_VEC, nm, sizeof nm);
            logf_("stance: unit %p refused \"%s\" (refused %u, melee picks %u)", *(void **)(ctx + 8), nm, st_reject, st_allow);
        }
        return NULL;
    }
    if (d == MELEE) {
        if (cfg_lift_ready) ctx[0x18] = 1;
        r = g_orig(ctx);
        ctx[0x18] = ready;
        if (r) {
            st_allow++;
            if (cfg_log && g_lines < 40) {
                g_lines++;
                char nm[200];
                vec_str(*(uint8_t **)ctx + cfg_namtags + TAGS_VEC, nm, sizeof nm);
                logf_("stance: unit %p takes \"%s\" (refused %u, melee picks %u)", *(void **)(ctx + 8), nm, st_reject, st_allow);
            }
        }
    } else r = g_orig(ctx);
    if (from_scan && r) charge_note(ctx, ready != 0, d == MELEE);
    return r;
}

/* ---------------------------------------------------------------- instant buttons on several units */
/* Vanilla runs an {instant} button only when exactly one selected unit can use it (GetActor). For actions with
 * {apply_to_all}, when GetActor finds none/several, run the vanilla ExecuteInstant once per eligible unit with
 * GetActor answering that unit (the action's vptr points at a copy of its vtable for the call), so each unit gets
 * exactly what a single-unit click gives it. */
typedef void  (*exinst_t)(void *action, void *list);
typedef void *(*getactor_t)(void *action, void *list);
typedef void *(*expand_t)(void *out, void *list);
typedef uint8_t (*canapply_t)(void *action, void *unit, void *zero);
static int cfg_multi = 1;
static exinst_t g_exinst;
static uint32_t g_off_apply, g_off_getactor, g_off_canapply;
static expand_t g_expand;
static void *g_cur_actor;
static uint32_t st_multi_clicks, st_multi_units;
#define VTCOPY 64
static struct { void **orig; void **copy; } g_vtc[16];

__attribute__((used, ms_abi)) void *ms_getactor_cur(void *action, void *list) { (void)action; (void)list; return g_cur_actor; }

static void **vt_copy(void **vt)
{
    for (int i = 0; i < 16; i++) if (g_vtc[i].orig == vt) return g_vtc[i].copy;
    for (int i = 0; i < 16; i++)
        if (!g_vtc[i].orig) {
            void **c = (void **)VirtualAlloc(NULL, VTCOPY * 8, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            if (!c) return NULL;
            memcpy(c, vt, VTCOPY * 8);
            c[g_off_getactor / 8] = (void *)ms_getactor_cur;
            g_vtc[i].orig = vt; g_vtc[i].copy = c;
            return c;
        }
    return NULL;
}

/* Draw the melee weapon when the stance turns ON: melee_toggle.set has {item "melee"} {item_in_box} and
 * {item "melee"}; for a unit being switched on (not tagged with the stance tags) that does not hold a melee weapon (no
 * charge tag), the dll gives it the engine's take-item order for the action's {item} tags (Order::eTakeItem, the order
 * `{take_item}` builds: the held item already matches = done, else the first matching inventory item goes TakeToRight)
 * + AddOrder(unit, order, {1, stamp}). 0.6.3 ran the action's {instructions} through CreateOrder instead, but
 * eCustomActionOrder first checks the click target (action vt+0x58: no {target} = fails) and silently drops itself.
 * Button name = std::string at action+0x60 (confirmed in game). */
typedef void *(*pickctor_t)(void *buf);
typedef void *(*takeitem_t)(void *itemtags);
static pickctor_t g_pickctor;
static takeitem_t g_takeitem;
static uint32_t g_off_item;
static char cfg_drawbtn[TLEN] = "melee_toggle";
static int cfg_draw = 1, g_names_logged, g_dlines;
static uint32_t cfg_draw_cd = 3000;            /* ms: no second take-item order for a unit while the first may still be running */
static uint8_t *g_draw_action;                 /* the melee_toggle action, remembered from its first click (actions live for the whole session) */
static uint32_t st_draws, st_draw_drop, st_draw_wait;
#define OFF_BUTTONNAME  0x60

static int is_draw_action(uint8_t *action)
{
    if (!cfg_draw || !g_takeitem || !g_ordstamp || !g_addorder) return 0;
    size_t n; const char *s = str_get(action + OFF_BUTTONNAME, &n);
    if (s && eq_ci(s, n, cfg_drawbtn)) {
        if (cfg_log && !g_names_logged) { g_names_logged = 1; logf_("draw: toggle action %p found (button \"%.*s\", {item} tags +0x%X)", (void *)action, (int)n, s, g_off_item); }
        g_draw_action = action; return 1;
    }
    return 0;
}
static int wants_draw(const uint8_t *u)
{
    return u && (u[0x30] & 0x04) && vec_has(u + cfg_enttags, g_tagl, n_tagl) != 1 && vec_has(u + cfg_enttags, g_ctagl, 1) != 1;
}
/* 1 = a take-item order was added, 0 = skipped (linked, cooldown) or refused. Re-adding the order restarts the take
 * (stage 0 clears the running take op), so a unit gets at most one per DrawCooldownMs. */
static int draw_melee(uint8_t *action, void **vt, uint8_t *u)
{
    (void)vt;
    if (!g_takeitem || !g_addorder || !u || is_linked(u)) return 0;
    uint32_t now = g_clock ? *g_clock : 0;
    last_t *l = now ? last_of(u, 1) : NULL;
    if (l && l->draw_ms && (uint32_t)(now - l->draw_ms) < cfg_draw_cd) { st_draw_wait++; return 0; }
    void *o = g_takeitem(action + g_off_item);
    uint32_t params[2] = { 1, *g_ordstamp };
    void *ok = o ? g_addorder(u, o, params) : NULL;
    if (ok) { st_draws++; if (l) l->draw_ms = now; } else st_draw_drop++;
    if (cfg_log && g_dlines < 40) {
        g_dlines++;
        logf_("draw: [%u ms] unit %p take-item order %p %s (draws %u, refused %u, waited %u)", now, (void *)u, o, ok ? "added" : "REFUSED by AddOrder", st_draws, st_draw_drop, st_draw_wait);
    }
    return ok != NULL;
}

__attribute__((used, ms_abi)) void ms_exinst(uint8_t *action, void *list)
{
    if (!cfg_multi || !action || !list || !action[g_off_apply]) { g_exinst(action, list); return; }
    void **vt = *(void ***)action;
    int drawact = is_draw_action(action);
    uint8_t *one = (uint8_t *)((getactor_t)vt[g_off_getactor / 8])(action, list);
    if (one) {                                                 /* vanilla single-unit case */
        int d = drawact && wants_draw(one);
        g_exinst(action, list);
        if (d) draw_melee(action, vt, one);
        return;
    }
    struct { void **b, **e, **c; } units = { 0, 0, 0 };
    g_expand(&units, list);                                    /* selection, squad card expanded (vector leaks, a few bytes per click) */
    if (!units.b || units.e <= units.b || units.e - units.b > 256) { g_exinst(action, list); return; }
    void **cp = vt_copy(vt);
    if (!cp) { g_exinst(action, list); return; }
    canapply_t can = (canapply_t)vt[g_off_canapply / 8];
    int n = 0;
    for (void **u = units.b; u < units.e; u++) {
        if (!*u || !can(action, *u, NULL)) continue;
        int d = drawact && wants_draw((uint8_t *)*u);
        g_cur_actor = *u;
        *(void ***)action = cp;
        g_exinst(action, list);
        *(void ***)action = vt;
        if (d) draw_melee(action, vt, (uint8_t *)*u);
        n++;
    }
    g_cur_actor = NULL;
    st_multi_clicks++; st_multi_units += n;
    if (cfg_log && g_lines < 40) {
        g_lines++;
        logf_("instant: button on %d of %d selected units (clicks %u, units %u)", n, (int)(units.e - units.b), st_multi_clicks, st_multi_units);
    }
}

/* ---------------------------------------------------------------- melee through a linked shield */
/* Before a paired blow starts, the engine casts a ray attacker -> target (query 0x6003, the bullet volumes) into a
 * global hit list (0x48-byte records sorted by distance, entity at +0x18) and drops the blow unless the FIRST hit
 * is the target (blow picker, game exe+0x95E5C2). A forcefield is its own entity linked to the wearer, so it is hit
 * first and the wearer can never be meleed. ms_ray_ok: walk the hits in order, skip entities linked to the target
 * (its shield, its kit) or carrying <SeeThroughTag>, then the target must come next; anything else still blocks. */
static int cfg_ray = 0;          /* off by default since 0.6.1 (Alex: 0.6 behaviour); [ShieldMelee] Enabled=1 turns the 0.7 ray patch back on */
static char cfg_seetag[1][TLEN] = { "forcefield" };
static uint8_t *g_ray_hits;
static uint32_t st_ray_pass, st_ray_skip, st_ray_block;

static const uint8_t *link_holder(const uint8_t *e)
{
    if (!((*(const uint32_t *)(e + cfg_linkoff) >> cfg_linkbit) & 1)) return NULL;
    const uint8_t *lk = *(const uint8_t *const *)(e + 0x560);
    return lk ? *(const uint8_t *const *)(lk + 8) : NULL;
}

__attribute__((used, ms_abi, force_align_arg_pointer)) int ms_ray_ok(const uint8_t *target)
{
    const uint8_t *b = *(const uint8_t *const *)g_ray_hits, *e = *(const uint8_t *const *)(g_ray_hits + 8);
    if (!b || e <= b || (uintptr_t)(e - b) % 0x48 || (uintptr_t)(e - b) > 0x48 * 256) return 0;
    if (*(const uint8_t *const *)(b + 0x18) == target) { st_ray_pass++; return 1; }   /* vanilla pass */
    if (!cfg_ray || !target) return 0;
    int skipped = 0;
    for (const uint8_t *r = b; r < e; r += 0x48) {
        const uint8_t *ent = *(const uint8_t *const *)(r + 0x18);
        if (ent == target) {
            st_ray_skip++;
            if (cfg_log && g_lines < 40) { g_lines++; logf_("melee ray: target %p reached through %d attached hit(s) (passed %u, through shield %u, blocked %u)", (void *)target, skipped, st_ray_pass, st_ray_skip, st_ray_block); }
            return 1;
        }
        if (!ent) break;
        const uint8_t *h = link_holder(ent);
        int tagged = vec_has(ent + cfg_enttags, cfg_seetag, 1) == 1;
        if (h == target || tagged) { skipped++; continue; }
        if (cfg_log && g_lines < 40) { g_lines++; logf_("melee ray: target %p blocked by %p (linked to %p, tag %d)", (void *)target, (void *)ent, (void *)h, tagged); }
        break;
    }
    st_ray_block++;
    return 0;
}

/* ---------------------------------------------------------------- finders */
#define W 0x100
/* body: movzx e?x,byte[r?+394h]; test ?l,1Ch  (row flags: enable_for_ai/user/human_user) */
static const uint16_t SIG_FLAGS[] = { 0x0F, 0xB6, W, 0x94, 0x03, 0x00, 0x00, 0xF6, W, 0x1C };
/* prologue: mov [rsp+20h],rbx; push rbp; push rsi; push rdi; push r12; push r13; push r14; push r15 */
static const uint8_t PROLOG[] = { 0x48, 0x89, 0x5C, 0x24, 0x20, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57 };
#define PATCH 14       /* through push r14 */
/* ammo {tag} offset: add rcx,imm32 ; call */
static const uint16_t SIG_ADDTAGS[] = { 0x48, 0x81, 0xC1, W, W, 0x00, 0x00, 0xE8 };
/* linked bit: mov eax,[r?+1C0h]; shr eax,1Ch; test al,1 */
static const uint16_t SIG_LINK[] = { 0x8B, W, 0xC0, 0x01, 0x00, 0x00, 0xC1, 0xE8, 0x1C, 0xA8, 0x01 };
/* range: movaps xmm6,xmm0; call Range; comiss xmm0,xmm6 */
static const uint16_t SIG_RANGE[] = { 0x0F, 0x28, 0xF0, 0xE8, W, W, W, W, 0x0F, 0x2F, 0xC6 };
/* MakeAttack: mov byte [rbx+0C1h],dil (unique) */
static const uint16_t SIG_C1[] = { 0x40, 0x88, 0xBB, 0xC1, 0x00, 0x00, 0x00 };
/* after MakeAttack: mov rdx,rax; lea r8,[rsp+20h]; mov rcx,r64; call AddOrder */
static const uint16_t SIG_ADD[] = { 0x48, 0x8B, 0xD0, 0x4C, 0x8D, 0x44, 0x24, 0x20, 0x48, 0x8B, W, 0xE8 };
/* MakeRef: lea r8,[rsi+30h]; mov rdx,rbx; mov rcx,rdi; call SetEntity */
static const uint16_t SIG_SETENT[] = { 0x4C, 0x8D, 0x46, 0x30, 0x48, 0x8B, 0xD3, 0x48, 0x8B, 0xCF, 0xE8 };
/* MakeRef: mov rcx,rdi; call RefInit */
static const uint16_t SIG_REFINIT[] = { 0x48, 0x8B, 0xCF, 0xE8 };
/* MakeRef: lea rcx,[rdi+20h] */
static const uint16_t SIG_TAG20[] = { 0x48, 0x8D, 0x4F, 0x20 };
/* sim clock: brain timers `mov ebx,[rdi+X]; call GetClock; cmp eax,ebx; jb` (from GEM BoardFilter) */
static const uint16_t SIG_CLK[] = { 0x8B, 0x9F, W, W, 0x00, 0x00, 0xE8, W, W, W, W, 0x3B, 0xC3, 0x0F, 0x82 };

static int match(const uint8_t *p, const uint16_t *sig, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (sig[i] != W && p[i] != sig[i]) return 0;
    return 1;
}
#define MATCH(p, s) match(p, s, sizeof s / 2)
static uint8_t *rel32(uint8_t *at) { return at + 4 + *(int32_t *)at; }

typedef struct {
    uint8_t *fn; uint32_t ammotags; int linksites;
    /* charge */
    uint8_t *mkmove;
    uint8_t *fight, *mvchange, *uimove;      /* 0.6.9: eFight factory, eMovementChange factory, the UI click-move factory */
    uint8_t *range, *scan_lo, *scan_hi, *mkatk, *addorder, *mkref, *setent, *refinit, *tagctor, *refvt,
            *refdtor, *tagdtor, *ordstamp, *clock;
    const char *charge_err;
    /* blow reach table (near/medium/far) */
    float *blowtab; uint8_t *blowfn;
    /* instant buttons on a multi-unit selection */
    uint8_t *exinst, *getactor, *expand, *pickctor, *takeitem; uint32_t off_apply, off_getactor, off_canapply, off_item;
    uint8_t **exslots[32]; int nexslots; const char *inst_err; int64_t vt_delta;
    /* melee line-of-reach ray (blow start) */
    uint8_t *ray_site, *ray_ok, *ray_fail, *ray_hits; int ray_reg; const char *ray_err;
} sites_t;

typedef struct { uint8_t *base, *lo, *hi; RUNTIME_FUNCTION *rf; size_t nrf; } img_t;

static RUNTIME_FUNCTION *func_of(img_t *im, uint8_t *p)
{
    uint32_t rva = (uint32_t)(p - im->base);
    size_t a = 0, b = im->nrf;
    while (a < b) { size_t m = (a + b) / 2; if (im->rf[m].BeginAddress <= rva) a = m + 1; else b = m; }
    if (!a) return NULL;
    RUNTIME_FUNCTION *f = &im->rf[a - 1];
    return rva < f->EndAddress ? f : NULL;
}
static int in_text(img_t *im, uint8_t *p) { return p >= im->lo && p < im->hi; }

/* next E8 call target at or after p within n bytes (NULL if none) */
static uint8_t *next_call(img_t *im, uint8_t *p, int n, uint8_t **at)
{
    for (int i = 0; i < n; i++)
        if (p[i] == 0xE8) {
            uint8_t *t = rel32(p + i + 1);
            if (in_text(im, t)) { if (at) *at = p + i; return t; }
        }
    return NULL;
}

static const char *find_charge(img_t *im, sites_t *s)
{
    uint8_t *lo = im->lo, *hi = im->hi;
    /* range call inside MatchAmmo */
    RUNTIME_FUNCTION *mf = func_of(im, s->fn);
    for (uint8_t *p = s->fn; p + 11 <= im->base + mf->EndAddress; p++)
        if (MATCH(p, SIG_RANGE)) { s->range = rel32(p + 4); break; }
    if (!s->range || !in_text(im, s->range)) return "range call not found in the ammo match";
    /* enemy scan = the only function that calls MatchAmmo twice */
    RUNTIME_FUNCTION *cand[16]; int cnt[16], nc = 0;
    for (uint8_t *p = lo; p + 5 <= hi; p++) {
        if (*p != 0xE8 || rel32(p + 1) != s->fn) continue;
        RUNTIME_FUNCTION *f = func_of(im, p);
        if (!f) continue;
        int i = 0; while (i < nc && cand[i] != f) i++;
        if (i == nc) { if (nc == 16) continue; cand[nc] = f; cnt[nc++] = 0; }
        cnt[i]++;
    }
    RUNTIME_FUNCTION *scan = NULL;
    for (int i = 0; i < nc; i++) if (cnt[i] == 2) { if (scan) return "enemy scan not unique"; scan = cand[i]; }
    if (!scan) return "enemy scan not found";
    s->scan_lo = im->base + scan->BeginAddress; s->scan_hi = im->base + scan->EndAddress;
    /* MakeAttack: function holding the unique +0xC1 flag store */
    uint8_t *c1 = NULL; int nc1 = 0;
    for (uint8_t *p = lo; p + 7 <= hi; p++) if (MATCH(p, SIG_C1)) { c1 = p; nc1++; }
    if (nc1 != 1) return "attack order factory not found";
    RUNTIME_FUNCTION *af = func_of(im, c1);
    if (!af) return "attack order factory has no function entry";
    s->mkatk = im->base + af->BeginAddress;
    /* its UI call site: ... call MakeAttack; [nop] mov rdx,rax; lea r8,[rsp+20h]; mov rcx,r; call AddOrder */
    uint8_t *site = NULL;
    for (uint8_t *p = lo; p + 24 <= hi; p++) {
        if (*p != 0xE8 || rel32(p + 1) != s->mkatk) continue;
        uint8_t *q = p + 5; while (*q == 0x90) q++;
        if (!MATCH(q, SIG_ADD)) continue;
        if (site) return "attack command site not unique";
        site = p;
        s->addorder = rel32(q + 12);
    }
    if (!site || !in_text(im, s->addorder)) return "attack command site not found";
    /* MakeRef: the last call before MakeAttack (within 0x40) whose body has the SetEntity sequence */
    for (uint8_t *p = site - 0x40; p < site; p++) {
        if (*p != 0xE8) continue;
        uint8_t *f = rel32(p + 1);
        if (!in_text(im, f)) continue;
        for (int i = 0; i < 0x70; i++)
            if (MATCH(f + i, SIG_SETENT)) { s->mkref = f; s->setent = rel32(f + i + 11); break; }
    }
    if (!s->mkref) return "target ref builder not found";
    uint8_t *mr = s->mkref, *at;
    for (int i = 0; i < 0x60 && !s->refvt; i++)          /* lea rax,[rip+vt]; mov [rdi],rax */
        if (mr[i] == 0x48 && mr[i + 1] == 0x8D && mr[i + 2] == 0x05 && mr[i + 7] == 0x48 && mr[i + 8] == 0x89 && mr[i + 9] == 0x07)
            s->refvt = rel32(mr + i + 3);
    for (int i = 0; i < 0x60 && !s->tagctor; i++)
        if (MATCH(mr + i, SIG_TAG20)) s->tagctor = next_call(im, mr + i + 4, 16, NULL);
    for (int i = 0; i < 0x70 && !s->refinit; i++)
        if (MATCH(mr + i, SIG_REFINIT) && in_text(im, rel32(mr + i + 4)) && rel32(mr + i + 4) != s->tagctor) s->refinit = rel32(mr + i + 4);
    if (!s->refvt || !s->tagctor || !s->refinit) return "target ref builder layout unexpected";
    /* after AddOrder: RefDtor then TagDtor */
    uint8_t *q = site + 5; while (*q == 0x90) q++;
    q += 17;
    s->refdtor = next_call(im, q, 0x30, &at);
    if (!s->refdtor) return "ref destructor not found";
    s->tagdtor = next_call(im, at + 5, 0x20, NULL);
    if (!s->tagdtor) return "tag destructor not found";
    /* order stamp: mov e?x,[rip+g] within 0x40 bytes before MakeAttack */
    for (uint8_t *p = site - 0x40; p < site; p++)
        if (p[0] == 0x8B && (p[1] & 0xC7) == 0x05 && p[-1] != 0x0F) {
            uint8_t *g = rel32(p + 2);
            if (g >= hi && g < im->base + 0x4000000) s->ordstamp = g;
        }
    if (!s->ordstamp) return "order stamp global not found";
    /* AI clock */
    uint8_t *cg[8] = { 0 }; int cc[8] = { 0 }, ncg = 0;
    for (uint8_t *p = lo; p + sizeof SIG_CLK / 2 <= hi; p++) {
        if (!MATCH(p, SIG_CLK)) continue;
        uint8_t *t = rel32(p + 7);
        if (t < lo || t + 7 > hi || t[0] != 0x8B || t[1] != 0x05 || t[6] != 0xC3) continue;
        uint8_t *g = rel32(t + 2);
        int i = 0; while (i < ncg && cg[i] != g) i++;
        if (i == ncg) { if (ncg == 8) continue; cg[ncg++] = g; }
        cc[i]++;
    }
    int best = -1;
    for (int i = 0; i < ncg; i++) if (best < 0 || cc[i] > cc[best]) best = i;
    if (best < 0 || cc[best] < 5) return "AI clock not found";
    s->clock = cg[best];
    /* eFight factory: the attack_melee action's "call Fight; mov r8,[rsp+50h]; mov rdx,rax; mov rcx,rbx; call AddOrder" */
    for (uint8_t *p = lo; p + 21 <= hi; p++) {
        if (p[0] != 0xE8 || memcmp(p + 5, "\x4C\x8B\x44\x24\x50\x48\x8B\xD0\x48\x8B\xCB\xE8", 12)) continue;
        if (rel32(p + 17) != s->addorder) continue;
        uint8_t *f = rel32(p + 1);
        if (!in_text(im, f)) continue;
        int alloc = 0;
        for (int i = 0; i < 0x40; i++) if (!memcmp(f + i, "\xB9\xE0\x00\x00\x00", 5)) { alloc = 1; break; }
        if (!alloc) continue;
        if (s->fight && s->fight != f) { s->fight = NULL; break; }
        s->fight = f;
    }
    /* eMovementChange factory: every "mov edx,[rax+rdx*4]; call X" (the mode table lookups) must agree, X allocates 0x48 */
    for (uint8_t *p = lo; p + 8 <= hi; p++) {
        if (memcmp(p, "\x8B\x14\x90\xE8", 4)) continue;
        uint8_t *f = rel32(p + 4);
        if (!in_text(im, f)) continue;
        int alloc = 0;
        for (int i = 0; i < 0x60; i++) if (!memcmp(f + i, "\xB9\x48\x00\x00\x00", 5)) { alloc = 1; break; }
        if (!alloc) continue;
        if (s->mvchange && s->mvchange != f) { s->mvchange = NULL; break; }
        s->mvchange = f;
    }
    /* UI click-move factory: alloc 0xF8, then within 0x40 bytes "movsd [rax+5Ch],xmm0", "mov qword [rax+40h],0", "mov [rax+38h],bl" */
    for (uint8_t *p = lo; p + 0x50 <= hi; p++) {
        if (memcmp(p, "\xB9\xF8\x00\x00\x00\xE8", 6)) continue;
        int a = 0, b = 0, c = 0;
        for (int i = 6; i < 0x40; i++) {
            if (!memcmp(p + i, "\xF2\x0F\x11\x40\x5C", 5)) a = 1;
            if (!memcmp(p + i, "\x48\xC7\x40\x40\x00\x00\x00\x00", 8)) b = 1;
            if (!memcmp(p + i, "\x88\x58\x38", 3)) c = 1;
        }
        if (!(a && b && c)) continue;
        RUNTIME_FUNCTION *rf = func_of(im, p);
        if (!rf) continue;
        uint8_t *f = im->base + rf->BeginAddress;
        if (s->uimove && s->uimove != f) { s->uimove = NULL; break; }
        s->uimove = f;
    }
    return NULL;
}

/* Action::ExecuteInstant (action vtable slot, game exe+0xB5E260): runs the {instant} block on the ONE unit
 * GetActor returns (vtable +0xF0, game exe+0xB73E90: expands the selection, returns a unit only when exactly
 * one passes CanApply = vtable +0xD8). Body: mov rax,[rcx+0x870]; cmp [rcx+0x868],rax; ...; call [rax+0xF0]. */
static const char *find_instant(img_t *im, sites_t *s)
{
    uint8_t *f = NULL; int n = 0;
    for (uint8_t *p = im->lo; p + 0x40 <= im->hi; p++) {
        /* cmp qword [rcx+0x868],rax */
        if (!(p[0] == 0x48 && p[1] == 0x39 && p[2] == 0x81 && *(uint32_t *)(p + 3) == 0x868)) continue;
        int has_call = 0;
        for (int i = 0; i < 0x30; i++) if (p[i] == 0xFF && p[i + 1] == 0x90 && *(uint32_t *)(p + i + 2) == 0xF0) { has_call = 1; break; }
        if (!has_call) continue;
        RUNTIME_FUNCTION *rf = func_of(im, p);
        if (!rf || p - (im->base + rf->BeginAddress) > 0x40) continue;
        uint8_t *fn = im->base + rf->BeginAddress;
        if (fn == f) continue;
        f = fn; n++;
    }
    if (n != 1) return n ? "ExecuteInstant not unique" : "ExecuteInstant not found";
    s->exinst = f; s->off_getactor = 0xF0;
    for (int i = 0; i < 0x140 && !s->pickctor; i++)              /* first direct call in ExecuteInstant = pick ctor */
        if (f[i] == 0xE8 && in_text(im, rel32(f + i + 1))) s->pickctor = rel32(f + i + 1);
    /* vtable slots holding it (outside .text) */
    IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)(im->base + ((IMAGE_DOS_HEADER *)im->base)->e_lfanew);
    uint8_t *end = im->base + nt->OptionalHeader.SizeOfImage;
    uint64_t want = (uint64_t)(uintptr_t)f;
    uint64_t want2 = (uint64_t)(f - im->base) + nt->OptionalHeader.ImageBase;   /* image mapped without relocations (offline test) */
    s->vt_delta = (int64_t)(want2 - want);
    uint8_t **apply_slot = NULL;
    for (uint8_t *p = (uint8_t *)(((uintptr_t)im->hi + 7) & ~(uintptr_t)7); p + 8 <= end && s->nexslots < 32; p += 8) {
        uint64_t v = *(uint64_t *)p;
        if (v != want && v != want2) continue;
        if (v == want) s->vt_delta = 0;
        s->exslots[s->nexslots++] = (uint8_t **)p;
        if (!apply_slot) apply_slot = (uint8_t **)(p - 8);
    }
    if (!s->nexslots) return "no vtable holds ExecuteInstant";
    /* apply_to_all byte: the previous slot (Apply, game exe+0xB740F0) opens with cmp byte [rcx+X],0 */
#define VTP(x) ((uint8_t *)((uint64_t)(uintptr_t)(x) - (uint64_t)s->vt_delta))
    uint8_t *ap = VTP(*apply_slot);
    if (!in_text(im, ap)) return "Apply slot not code";
    for (int i = 0; i < 0x30; i++)
        if (ap[i] == 0x80 && ap[i + 1] == 0xB9 && ap[i + 6] == 0x00) { s->off_apply = *(uint32_t *)(ap + i + 2); break; }
    if (s->off_apply < 0x100 || s->off_apply > 0x2000) return "apply_to_all offset not found";
    /* GetActor = vtable[+0xF0]; its first call = selection expand; its call [rax+X] = CanApply */
    uint8_t **vt = s->exslots[0] - 19;                        /* slot 19 in every build seen; checked below */
    if (VTP(vt[19]) != f) return "vtable layout unexpected";
    s->getactor = VTP(vt[s->off_getactor / 8]);
    if (!in_text(im, s->getactor)) return "GetActor slot not code";
    for (int i = 0; i < 0x60 && !s->expand; i++)
        if (s->getactor[i] == 0xE8 && in_text(im, rel32(s->getactor + i + 1))) s->expand = rel32(s->getactor + i + 1);
    for (int i = 0; i < 0x90; i++)
        if (s->getactor[i] == 0xFF && s->getactor[i + 1] == 0x90 && *(uint32_t *)(s->getactor + i + 2) < 0x400) {
            s->off_canapply = *(uint32_t *)(s->getactor + i + 2); break;
        }
    if (!s->expand || !s->off_canapply) return "GetActor layout unexpected";
    return NULL;
}

/* Order::eTakeItem factory (game exe+0x8EA160, (Tags *item) -> order): the `{take_item}` order instruction's exec
 * (instruction vtable slot 3, game exe+0xB5DC30) = lea rcx,[rdx+X] (X = the action's {item} Tags, +0xC8); call it.
 * The instruction table in .rdata pairs the name pointer "take_item" with the factory (lea rcx,[rip+vt]). */
static const char *find_takeitem(img_t *im, sites_t *s)
{
    IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)(im->base + ((IMAGE_DOS_HEADER *)im->base)->e_lfanew);
    uint8_t *end = im->base + nt->OptionalHeader.SizeOfImage;
    static const char nm[] = "take_item";
    uint8_t *str = NULL;
    for (uint8_t *p = im->hi; p + sizeof nm <= end; p++)
        if (p[-1] == 0 && !memcmp(p, nm, sizeof nm)) { str = p; break; }
    if (!str) return "take_item name not found";
    uint64_t w1 = (uint64_t)(uintptr_t)str, w2 = (uint64_t)(str - im->base) + nt->OptionalHeader.ImageBase;
    for (uint8_t *p = (uint8_t *)(((uintptr_t)im->hi + 7) & ~(uintptr_t)7); p + 16 <= end; p += 8) {
        uint64_t v = *(uint64_t *)p;
        if (v != w1 && v != w2) continue;
        int64_t dl = v == w1 ? 0 : (int64_t)(w2 - w1);
        uint8_t *fac = (uint8_t *)(uintptr_t)(*(uint64_t *)(p + 8) - (uint64_t)dl);
        if (!in_text(im, fac)) continue;
        for (int i = 0; i < 0x30; i++) {
            if (!(fac[i] == 0x48 && fac[i + 1] == 0x8D && fac[i + 2] == 0x0D)) continue;   /* lea rcx,[rip+vt] */
            uint8_t **vt = (uint8_t **)rel32(fac + i + 3);
            if ((uint8_t *)vt < im->hi || (uint8_t *)vt + 0x20 > end) break;
            uint8_t *ex = (uint8_t *)(uintptr_t)((uint64_t)(uintptr_t)vt[3] - (uint64_t)dl);
            if (!in_text(im, ex)) break;
            for (int j = 0; j < 0x20; j++)
                if (ex[j] == 0x48 && ex[j + 1] == 0x8D && ex[j + 2] == 0x8A) {           /* lea rcx,[rdx+X] */
                    s->off_item = *(uint32_t *)(ex + j + 3);
                    s->takeitem = next_call(im, ex + j + 7, 0x10, NULL);
                    break;
                }
            break;
        }
        if (s->takeitem) break;
    }
    if (!s->takeitem || s->off_item < 0x40 || s->off_item > 0x1000) return "take_item order factory not found";
    return NULL;
}

static const char *find_all(uint8_t *base, sites_t *s)
{
    IMAGE_DOS_HEADER *d = (IMAGE_DOS_HEADER *)base;
    IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)(base + d->e_lfanew);
    IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
    img_t im = { base, NULL, NULL, NULL, 0 };
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++, sec++)
        if (!memcmp(sec->Name, ".text", 5)) { im.lo = base + sec->VirtualAddress; im.hi = im.lo + sec->Misc.VirtualSize; break; }
    if (!im.lo) return "no .text section";
    IMAGE_DATA_DIRECTORY *ex = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    if (!ex->VirtualAddress || !ex->Size) return "no exception directory";
    im.rf = (RUNTIME_FUNCTION *)(base + ex->VirtualAddress);
    im.nrf = ex->Size / sizeof *im.rf;

    memset(s, 0, sizeof *s);
    int found = 0;
    for (uint8_t *p = im.lo; p + sizeof SIG_FLAGS / 2 <= im.hi; p++) {
        if (!MATCH(p, SIG_FLAGS)) continue;
        RUNTIME_FUNCTION *f = func_of(&im, p);
        if (!f) continue;
        uint8_t *fn = base + f->BeginAddress, *fe = base + f->EndAddress;
        if (memcmp(fn, PROLOG, sizeof PROLOG)) continue;
        if (fe - fn > 0x600) continue;
        uint32_t off = 0;
        for (uint8_t *q = fn; q + sizeof SIG_ADDTAGS / 2 <= fe; q++)
            if (MATCH(q, SIG_ADDTAGS)) { off = *(uint32_t *)(q + 3); break; }
        if (off < 0x400 || off > 0x3000) continue;
        if (s->fn == fn) continue;
        if (s->fn) return "ammo match function not unique";
        s->fn = fn; s->ammotags = off; found++;
    }
    if (!found) return "ammo match function not found (unknown exe build, or already patched)";
    for (uint8_t *p = im.lo; p + sizeof SIG_LINK / 2 <= im.hi; p++)
        if (MATCH(p, SIG_LINK)) s->linksites++;
    s->charge_err = find_charge(&im, s);
    s->inst_err = find_instant(&im, s);
    if (!s->inst_err) s->inst_err = find_takeitem(&im, s);
    /* MakeMove(xy, flag): the function holding the "Moving to zero bug!" assert (game exe+0x8F3600,
     * editor 0x779730 opens mov [rsp+10h],rbx, server 0x708330) */
    {
        IMAGE_NT_HEADERS64 *nt2 = (IMAGE_NT_HEADERS64 *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
        uint8_t *end = base + nt2->OptionalHeader.SizeOfImage, *str = NULL;
        static const char msg[] = "Moving to zero bug!";
        for (uint8_t *q = im.hi; q + sizeof msg + 1 <= end && !str; q++)
            if (q[0] == 0 && !memcmp(q + 1, msg, sizeof msg)) str = q + 1;
        uint8_t *ref = NULL;
        for (uint8_t *q = im.lo; str && q + 7 <= im.hi && !ref; q++)
            if ((q[0] == 0x48 || q[0] == 0x4C) && q[1] == 0x8D && (q[2] & 0xC7) == 0x05 && rel32(q + 3) == str) ref = q;
        RUNTIME_FUNCTION *rf = ref ? func_of(&im, ref) : NULL;
        uint8_t *f = rf ? base + rf->BeginAddress : NULL;
        if (f && ref - f < 0x100 && f[0] == 0x48 && f[1] == 0x89 && f[2] == 0x5C && f[3] == 0x24 && (f[4] == 0x08 || f[4] == 0x10)) s->mkmove = f;
    }
    /* blow start ray check: mov rax,[rip+hits]; cmp rax,[rip+hits+8]; je fail; cmp [rax+18h],rTarget; jne fail */
    {
        int n = 0;
        for (uint8_t *p = im.lo; p + 30 <= im.hi; p++) {
            if (p[0] != 0x48 || p[1] != 0x8B || p[2] != 0x05 || p[7] != 0x48 || p[8] != 0x3B || p[9] != 0x05) continue;
            if (p[14] != 0x0F || p[15] != 0x84 || p[20] != 0x4C || p[21] != 0x39 || (p[22] & 0xC7) != 0x40 || p[23] != 0x18) continue;
            if (p[24] != 0x0F || p[25] != 0x85) continue;
            uint8_t *g1 = rel32(p + 3), *g2 = rel32(p + 10);
            if (g2 != g1 + 8) continue;
            uint8_t *f1 = rel32(p + 16), *f2 = rel32(p + 26);
            if (f1 != f2) continue;
            n++;
            s->ray_site = p; s->ray_hits = g1; s->ray_fail = f1; s->ray_ok = p + 30; s->ray_reg = 8 + ((p[22] >> 3) & 7);
        }
        s->ray_err = n == 1 ? NULL : (n ? "melee ray check not unique" : "melee ray check not found");
    }
    /* blow reach: GetBlowDistance = movsxd rax,ecx; lea r,[rip+table]; movss xmm0,[r+rax*4]; ret,
     * table holds the vanilla 17 / 29 / 35 (near / medium / far, world units) */
    for (uint8_t *p = im.lo; p + 17 <= im.hi; p++) {
        if (p[0] != 0x48 || p[1] != 0x63 || p[2] != 0xC1 || p[3] != 0x48 || p[4] != 0x8D || (p[5] & 0xC7) != 0x05) continue;
        int reg = (p[5] >> 3) & 7;
        if (p[10] != 0xF3 || p[11] != 0x0F || p[12] != 0x10 || p[13] != 0x04 || p[14] != (0x80 | reg) || p[15] != 0xC3) continue;
        float *t = (float *)rel32(p + 6);
        if (t[0] != 17.0f || t[1] != 29.0f || t[2] != 35.0f) continue;
        if (s->blowfn) { s->blowfn = NULL; s->blowtab = NULL; break; }   /* not unique: leave it alone */
        s->blowfn = p; s->blowtab = t;
    }
    return NULL;
}

/* ---------------------------------------------------------------- ini */
static int split_list(const char *src, char out[][TLEN], int max)
{
    int n = 0; const char *p = src;
    while (*p && n < max) {
        while (*p == ' ' || *p == ',' || *p == '\t') p++;
        int k = 0;
        while (*p && *p != ',' && *p != ' ' && *p != '\t' && k < TLEN - 1) out[n][k++] = lc(*p++);
        while (*p && *p != ',' && *p != ' ' && *p != '\t') p++;
        out[n][k] = 0;
        if (k) n++;
    }
    return n;
}

static void load_ini(const char *ini)
{
    char buf[1024];
    cfg_enabled     = GetPrivateProfileIntA("MeleeStance", "Enabled", 1, ini);
    cfg_log         = GetPrivateProfileIntA("MeleeStance", "Log", 1, ini);
    cfg_skip_linked = GetPrivateProfileIntA("MeleeStance", "SkipLinked", 1, ini);
    cfg_lift_ready  = GetPrivateProfileIntA("MeleeStance", "DrawFromBelt", 1, ini);
    GetPrivateProfileStringA("MeleeStance", "Tag", "melee_stance", cfg_tag, sizeof cfg_tag, ini);
    GetPrivateProfileStringA("MeleeStance", "MeleeAmmoTags",
        "melee_vs_human,melee_vs_all,medium_inf_melee_target,medium_veh_melee_target,heavy_veh_melee_target", buf, sizeof buf, ini);
    n_mtags = split_list(buf, cfg_mtags, MAXT);
    cfg_charge        = GetPrivateProfileIntA("Charge", "Enabled", 1, ini);
    GetPrivateProfileStringA("Charge", "Tag", "has_melee_weapon", cfg_ctag, sizeof cfg_ctag, ini);
    cfg_charge_inhand = GetPrivateProfileIntA("Charge", "MeleeInHand", 1, ini);
    cfg_charge_player = GetPrivateProfileIntA("Charge", "PlayerUnits", 1, ini);
    cfg_charge_cd     = (uint32_t)GetPrivateProfileIntA("Charge", "CooldownMs", 4000, ini);
    cfg_retarget      = (uint32_t)GetPrivateProfileIntA("Charge", "RetargetMs", 1500, ini);
    {
        char b2[64];
        GetPrivateProfileStringA("Charge", "SwitchRatio", "0.6", b2, sizeof b2, ini);
        cfg_switch = (float)atof(b2); if (cfg_switch < 0.1f) cfg_switch = 0.1f; if (cfg_switch > 1.0f) cfg_switch = 1.0f;
        GetPrivateProfileStringA("Charge", "ProgressUnits", "10", b2, sizeof b2, ini);
        cfg_progress = (float)atof(b2); if (cfg_progress < 0.0f) cfg_progress = 0.0f;
        GetPrivateProfileStringA("Charge", "CloseDistance", "200", b2, sizeof b2, ini);
        cfg_close = (float)atof(b2); if (cfg_close < 0.0f) cfg_close = 0.0f;
        cfg_runmode = GetPrivateProfileIntA("Charge", "RunMode", 2, ini); if (cfg_runmode > 3) cfg_runmode = 3;
        GetPrivateProfileStringA("Charge", "MoveRefreshUnits", "80", b2, sizeof b2, ini);
        cfg_refresh = (float)atof(b2); if (cfg_refresh < 10.0f) cfg_refresh = 10.0f;
    }
    GetPrivateProfileStringA("Charge", "MaxDistance", "2000", buf, sizeof buf, ini);
    cfg_charge_max = (float)atof(buf);
    GetPrivateProfileStringA("Charge", "MinDistance", "60", buf, sizeof buf, ini);
    cfg_charge_min = (float)atof(buf);
    cfg_multi = GetPrivateProfileIntA("MultiSelect", "Enabled", 1, ini);
    cfg_draw  = GetPrivateProfileIntA("MeleeStance", "DrawOnToggle", 1, ini);
    cfg_draw_cd = (uint32_t)GetPrivateProfileIntA("MeleeStance", "DrawCooldownMs", 3000, ini);
    GetPrivateProfileStringA("MeleeStance", "DrawButton", "melee_toggle", cfg_drawbtn, sizeof cfg_drawbtn, ini);
    cfg_ray = GetPrivateProfileIntA("ShieldMelee", "Enabled", 0, ini);
    GetPrivateProfileStringA("ShieldMelee", "SeeThroughTag", "forcefield", buf, sizeof buf, ini);
    split_list(buf, cfg_seetag, 1);
    cfg_down_on = GetPrivateProfileIntA("Downed", "Enabled", 1, ini);
    GetPrivateProfileStringA("Downed", "Tag", "start_personage", buf, sizeof buf, ini);
    split_list(buf, cfg_downtag, 1);
    cfg_blow_on = GetPrivateProfileIntA("BlowReach", "Enabled", 1, ini);
    static const char *bk[3] = { "Near", "Medium", "Far" }, *bd[3] = { "45", "45", "45" };
    for (int i = 0; i < 3; i++) {
        GetPrivateProfileStringA("BlowReach", bk[i], bd[i], buf, sizeof buf, ini);
        cfg_blow[i] = (float)atof(buf);
        if (cfg_blow[i] < 2.0f) cfg_blow[i] = 2.0f;           /* the engine skips targets closer than 2 units */
    }
    GetPrivateProfileStringA("MeleeStance", "AlwaysTag", "melee_always", cfg_atag, sizeof cfg_atag, ini);
}

static void init_tag(void)
{
    char tmp[1][TLEN];
    split_list(cfg_tag, g_tagl, 1);
    n_tagl = 1;
    if (split_list(cfg_atag, tmp, 1)) { strcpy(g_tagl[1], tmp[0]); n_tagl = 2; }
    split_list(cfg_ctag, g_ctagl, 1);
}

#ifndef MS_TEST
/* ---------------------------------------------------------------- install */
static uint8_t *emit(uint8_t *p, const void *b, size_t n) { memcpy(p, b, n); return p + n; }
static uint8_t *emit_absjmp(uint8_t *p, const void *to)
{
    static const uint8_t j[6] = { 0xFF, 0x25, 0, 0, 0, 0 };
    p = emit(p, j, 6);
    uint64_t t = (uint64_t)(uintptr_t)to;
    return emit(p, &t, 8);
}

static void install(void)
{
    uint8_t *base = (uint8_t *)GetModuleHandleW(NULL);
    sites_t s;
    init_tag();
    if (!cfg_enabled) { logf_("[GEM MeleeStance %s] Enabled=0, not installed", MS_VERSION); return; }
    const char *err = find_all(base, &s);
    if (err) { logf_("[GEM MeleeStance %s] NOT installed: %s", MS_VERSION, err); return; }
    if (s.linksites < 20) {
        cfg_skip_linked = 0;
        logf_("  linked flag test not found (%d sites), SkipLinked off: crews in stance would not fire", s.linksites);
    }
    cfg_ammotags = s.ammotags;
    if (s.clock) g_clock = (volatile uint32_t *)s.clock;     /* also used by the stance (melee carried check) */
    if (cfg_charge && s.charge_err) { cfg_charge = 0; logf_("  charge OFF: %s", s.charge_err); }
    if (cfg_charge) {
        g_range = (range_t)s.range; g_scan_lo = s.scan_lo; g_scan_hi = s.scan_hi;
        g_mkatk = (mkatk_t)s.mkatk; g_addorder = (addord_t)s.addorder; g_setent = (setent_t)s.setent;
        g_mkmove = (mkmove_t)s.mkmove;
        g_fight = (fight_t)s.fight; g_mvchange = (mvchange_t)s.mvchange; g_uimove = (uimove_t)s.uimove;
        if (!s.mkmove && !s.uimove) logf_("  charge: no move order factory found, the far part of the charge is the attack order too");
        else logf_("  charge: beyond CloseDistance=%.0f: %s (exe+0x%llX)%s, refreshed when the enemy moves %.0f; inside: %s (exe+0x%llX)",
                   cfg_close, s.mkmove ? "eMoveTask, the double-click run" : "eAdvance, the click walk", (unsigned long long)((s.mkmove ? s.mkmove : s.uimove) - base),
                   s.mvchange && cfg_runmode >= 0 ? " after eMovementChange" : " (no movement-mode order)", cfg_refresh,
                   s.fight ? "eFight melee order" : "attack order (eFight not found)", (unsigned long long)((s.fight ? s.fight : s.mkatk) - base));
        if (s.mvchange) logf_("  charge: eMovementChange exe+0x%llX, RunMode=%d", (unsigned long long)(s.mvchange - base), cfg_runmode);
        g_refinit = (vfn1_t)s.refinit; g_tagctor = (vfn1_t)s.tagctor; g_refvt = s.refvt;
        g_refdtor = (vfn1_t)s.refdtor; g_tagdtor = (vfn1_t)s.tagdtor;
        g_ordstamp = (volatile uint32_t *)s.ordstamp; g_clock = (volatile uint32_t *)s.clock;
    }

    uint8_t *tr = (uint8_t *)VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tr) { logf_("[GEM MeleeStance] VirtualAlloc failed (%lu)", GetLastError()); return; }
    uint8_t *q = emit(tr, s.fn, PATCH);
    emit_absjmp(q, s.fn + PATCH);
    FlushInstructionCache(GetCurrentProcess(), tr, 64);
    g_orig = (match_t)tr;

    uint8_t patch[PATCH];
    emit_absjmp(patch, (void *)ms_hook);
    DWORD old;
    if (!VirtualProtect(s.fn, PATCH, PAGE_EXECUTE_READWRITE, &old)) { logf_("[GEM MeleeStance] VirtualProtect failed (%lu)", GetLastError()); return; }
    memcpy(s.fn, patch, PATCH);
    VirtualProtect(s.fn, PATCH, old, &old);
    FlushInstructionCache(GetCurrentProcess(), s.fn, PATCH);

    char ml[600] = "";
    for (int i = 0; i < n_mtags; i++) { strcat(ml, cfg_mtags[i]); if (i + 1 < n_mtags) strcat(ml, ","); }
    logf_("[GEM MeleeStance %s] installed: ammo match exe+0x%llX, ammo tags +0x%X, linked +0x%X bit %u (%d sites) | "
          "Tag=%s AlwaysTag=%s MeleeAmmoTags=%s SkipLinked=%d DrawFromBelt=%d Log=%d",
          MS_VERSION, (unsigned long long)(s.fn - base), cfg_ammotags, cfg_linkoff, cfg_linkbit, s.linksites,
          g_tagl[0], n_tagl > 1 ? g_tagl[1] : "-", ml, cfg_skip_linked, cfg_lift_ready, cfg_log);
    if (cfg_charge)
        logf_("  charge ON: Tag=%s MeleeInHand=%d PlayerUnits=%d CooldownMs=%u RetargetMs=%u MinDistance=%.0f MaxDistance=%.0f | scan exe+0x%llX, "
              "MakeAttack exe+0x%llX, AddOrder exe+0x%llX, ref vt exe+0x%llX, range exe+0x%llX, clock exe+0x%llX, stamp exe+0x%llX",
              g_ctagl[0], cfg_charge_inhand, cfg_charge_player, cfg_charge_cd, cfg_retarget, cfg_charge_min, cfg_charge_max,
              (unsigned long long)(s.scan_lo - base), (unsigned long long)(s.mkatk - base), (unsigned long long)(s.addorder - base),
              (unsigned long long)(s.refvt - base), (unsigned long long)(s.range - base), (unsigned long long)(s.clock - base),
              (unsigned long long)(s.ordstamp - base));
    /* melee through linked shields: replace the blow picker's "first ray hit is the target" test with ms_ray_ok */
    if (cfg_ray) {
        if (s.ray_err) logf_("  melee through shields OFF: %s", s.ray_err);
        else {
            g_ray_hits = s.ray_hits;
            uint8_t *st = tr + 512, *q2 = st;
            /* mid-function site: rsp is already 16-aligned there (game/editor/server prologues), so reserve exactly
             * the 0x20 shadow space; 0x28 (right at a function entry) misaligned the call and crashed in the CRT */
            static const uint8_t sub28[] = { 0x48, 0x83, 0xEC, 0x20 }, add28[] = { 0x48, 0x83, 0xC4, 0x20 };
            static const uint8_t calrax[] = { 0xFF, 0xD0 }, testal[] = { 0x84, 0xC0, 0x74, 0x0E };
            uint8_t movrcx[3] = { (uint8_t)(s.ray_reg >= 8 ? 0x4C : 0x48), 0x89, (uint8_t)(0xC1 | ((s.ray_reg & 7) << 3)) };
            uint8_t movrax[2] = { 0x48, 0xB8 }; uint64_t fn = (uint64_t)(uintptr_t)ms_ray_ok;
            q2 = emit(q2, sub28, 4); q2 = emit(q2, movrcx, 3); q2 = emit(q2, movrax, 2); q2 = emit(q2, &fn, 8);
            q2 = emit(q2, calrax, 2); q2 = emit(q2, add28, 4); q2 = emit(q2, testal, 4);
            q2 = emit_absjmp(q2, s.ray_ok);                          /* al != 0: carry on with the blow */
            emit_absjmp(q2, s.ray_fail);                              /* al == 0: vanilla "blocked" path */
            FlushInstructionCache(GetCurrentProcess(), st, 64);
            uint8_t rp[30]; memset(rp, 0x90, sizeof rp);
            emit_absjmp(rp, st);
            DWORD o4;
            if (VirtualProtect(s.ray_site, 30, PAGE_EXECUTE_READWRITE, &o4)) {
                memcpy(s.ray_site, rp, 30);
                VirtualProtect(s.ray_site, 30, o4, &o4);
                FlushInstructionCache(GetCurrentProcess(), s.ray_site, 30);
                logf_("  melee through shields ON: blow ray test exe+0x%llX (hits exe+0x%llX, target r%d), skips hits linked to the target or tagged %s",
                      (unsigned long long)(s.ray_site - base), (unsigned long long)(s.ray_hits - base), s.ray_reg, cfg_seetag[0]);
            } else logf_("  melee through shields OFF: VirtualProtect failed (%lu)", GetLastError());
        }
    }
    /* instant buttons on several units: point every vtable slot of ExecuteInstant at ms_exinst */
    if (cfg_multi) {
        if (s.inst_err) logf_("  multi-unit instant buttons OFF: %s", s.inst_err);
        else {
            g_exinst = (exinst_t)s.exinst; g_expand = (expand_t)s.expand;
            g_pickctor = (pickctor_t)s.pickctor; g_ordstamp = (volatile uint32_t *)s.ordstamp;
            g_takeitem = (takeitem_t)s.takeitem; g_off_item = s.off_item;
            g_off_apply = s.off_apply; g_off_getactor = s.off_getactor; g_off_canapply = s.off_canapply;
            int ok = 0;
            for (int i = 0; i < s.nexslots; i++) {
                DWORD o3;
                if (!VirtualProtect(s.exslots[i], 8, PAGE_READWRITE, &o3)) continue;
                *s.exslots[i] = (uint8_t *)ms_exinst;
                VirtualProtect(s.exslots[i], 8, o3, &o3);
                ok++;
            }
            logf_("  multi-unit instant buttons ON ({apply_to_all} actions): ExecuteInstant exe+0x%llX in %d/%d vtables, "
                  "apply_to_all +0x%X, GetActor +0x%X (exe+0x%llX), CanApply +0x%X, expand exe+0x%llX",
                  (unsigned long long)(s.exinst - base), ok, s.nexslots, s.off_apply, s.off_getactor,
                  (unsigned long long)(s.getactor - base), s.off_canapply, (unsigned long long)(s.expand - base));
        }
    }
    /* blow reach: overwrite the engine's near/medium/far table */
    if (cfg_down_on) logf_("  downed targets ignored: no weapon is chosen against units tagged %s", cfg_downtag[0]);
    if (cfg_blow_on) {
        if (!s.blowtab) logf_("  blow reach OFF: table not found (unknown exe build, or already changed)");
        else {
            DWORD o2;
            if (VirtualProtect(s.blowtab, 12, PAGE_READWRITE, &o2)) {
                s.blowtab[0] = cfg_blow[0]; s.blowtab[1] = cfg_blow[1]; s.blowtab[2] = cfg_blow[2];
                VirtualProtect(s.blowtab, 12, o2, &o2);
                logf_("  blow reach ON: near/medium/far 17/29/35 -> %.1f/%.1f/%.1f units (table exe+0x%llX, GetBlowDistance exe+0x%llX)",
                      cfg_blow[0], cfg_blow[1], cfg_blow[2], (unsigned long long)((uint8_t *)s.blowtab - base),
                      (unsigned long long)(s.blowfn - base));
            } else logf_("  blow reach OFF: VirtualProtect failed (%lu)", GetLastError());
        }
    }
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID res)
{
    (void)res;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        char path[MAX_PATH], ini[MAX_PATH];
        DWORD n = GetModuleFileNameA(inst, path, MAX_PATH);
        if (n == 0 || n >= MAX_PATH - 16) return TRUE;
        char *dot = strrchr(path, '.');
        if (dot) *dot = 0;
        char exe[MAX_PATH], *nm;
        GetModuleFileNameA(NULL, exe, MAX_PATH);
        nm = strrchr(exe, '\\'); nm = nm ? nm + 1 : exe;
        snprintf(ini, sizeof ini, "%s.ini", path);
        snprintf(g_logpath, sizeof g_logpath, "%s%s.log", path,
                 strstr(nm, "_ed") ? "_editor" : (strstr(nm, "_server") ? "_server" : ""));
        load_ini(ini);
        install();
    }
    return TRUE;
}
#else
/* ---------------------------------------------------------------- offline test */
static void fake_str(uint8_t *p, const char *s)
{
    size_t n = strlen(s);
    memset(p, 0, 0x20);
    if (n <= 15) { memcpy(p, s, n); *(uint64_t *)(p + 0x18) = 15; }
    else { char *h = _strdup(s); *(char **)p = h; *(uint64_t *)(p + 0x18) = n; }
    *(uint64_t *)(p + 0x10) = n;
}
static void fake_vec(uint8_t *at, uint8_t *store, const char *a, const char *b, const char *c)
{
    const char *t[3] = { a, b, c }; int n = 0;
    for (int i = 0; i < 3; i++) if (t[i]) fake_str(store + 0x60 * n++ + 0x40, t[i]);
    *(uint8_t **)at = store; *(uint8_t **)(at + 8) = store + 0x60 * n;
}
typedef struct { uint8_t obj[0x1400]; uint8_t tags[0x60 * 4]; uint8_t names[0x60 * 4]; } fake_t;

static void *g_seen_ready;
static __attribute__((ms_abi)) void *fake_orig(uint8_t *ctx) { g_seen_ready = (void *)(uintptr_t)ctx[0x18]; return (void *)0x1234; }
/* fake engine for the charge path */
static uint32_t f_clock = 1000, f_stamp = 77;
static int f_orders; static void *f_last_unit, *f_last_tgt;
static __attribute__((ms_abi)) void f_nop(void *p) { (void)p; }
static __attribute__((ms_abi)) void f_setent(void *ref, void *ent, void *tag) { (void)tag; *(uint32_t *)((uint8_t *)ref + 8) = 1; *(void **)((uint8_t *)ref + 0x18) = ent; }
static __attribute__((ms_abi)) void *f_mkatk(void *ref, uint8_t flag) { (void)flag; f_last_tgt = *(void **)((uint8_t *)ref + 0x18); return (void *)0x99; }
static __attribute__((ms_abi)) void *f_add(void *a, void *o, void *pr) { f_orders++; f_last_unit = a; return ((uint32_t *)pr)[0] == 1 && ((uint32_t *)pr)[1] == 77 ? o : NULL; }
static __attribute__((ms_abi)) float f_range(void *s) { (void)s; return 3.5f; }

int main(int argc, char **argv)
{
    int bad = 0;
    for (int i = 1; i < argc; i++) {
        HMODULE m = LoadLibraryExA(argv[i], NULL, DONT_RESOLVE_DLL_REFERENCES);
        if (!m) { printf("%s: load failed %lu\n", argv[i], GetLastError()); bad = 1; continue; }
        uint8_t *base = (uint8_t *)m;
        sites_t s;
        const char *err = find_all(base, &s);
        if (err) { printf("%s: FAIL %s\n", argv[i], err); bad = 1; continue; }
        printf("%s\n  ammo match exe+0x%llX, ammo {tag} +0x%X, linked-flag sites %d\n", argv[i],
               (unsigned long long)(s.fn - base), s.ammotags, s.linksites);
        if (s.ammotags != 0xEC8 || s.linksites < 20) { printf("  FAIL unexpected offsets\n"); bad = 1; }
        if (s.ray_err) { printf("  FAIL shield melee: %s\n", s.ray_err); bad = 1; }
        else printf("  shield melee: ray test exe+0x%llX hits exe+0x%llX ok exe+0x%llX fail exe+0x%llX target r%d\n",
                    (unsigned long long)(s.ray_site - base), (unsigned long long)(s.ray_hits - base),
                    (unsigned long long)(s.ray_ok - base), (unsigned long long)(s.ray_fail - base), s.ray_reg);
        if (!s.takeitem) { printf("  FAIL take-item order factory not found\n"); bad = 1; }
        else printf("  draw: TakeItem order exe+0x%llX, action {item} +0x%X\n", (unsigned long long)(s.takeitem - base), s.off_item);
        if (s.inst_err) { printf("  FAIL multi-select: %s\n", s.inst_err); bad = 1; }
        else printf("  multi-select: ExecuteInstant exe+0x%llX in %d vtables, apply_to_all +0x%X, GetActor exe+0x%llX (+0x%X), CanApply +0x%X, expand exe+0x%llX\n",
                    (unsigned long long)(s.exinst - base), s.nexslots, s.off_apply, (unsigned long long)(s.getactor - base),
                    s.off_getactor, s.off_canapply, (unsigned long long)(s.expand - base));
        if (!s.blowtab) { printf("  FAIL blow reach table not found\n"); bad = 1; }
        else printf("  blow reach: table exe+0x%llX = %.0f/%.0f/%.0f, GetBlowDistance exe+0x%llX\n",
                    (unsigned long long)((uint8_t *)s.blowtab - base), s.blowtab[0], s.blowtab[1], s.blowtab[2],
                    (unsigned long long)(s.blowfn - base));
        if (!s.mkmove) { printf("  FAIL move order (MakeMove) not found\n"); bad = 1; }
        else printf("  move: MakeMove exe+0x%llX\n", (unsigned long long)(s.mkmove - base));
        if (!s.fight) { printf("  FAIL eFight (attack_melee order) factory not found\n"); bad = 1; }
        else printf("  melee: eFight exe+0x%llX\n", (unsigned long long)(s.fight - base));
        if (!s.mvchange) { printf("  FAIL eMovementChange factory not found\n"); bad = 1; }
        else printf("  mode: eMovementChange exe+0x%llX\n", (unsigned long long)(s.mvchange - base));
        if (!s.uimove) { printf("  FAIL UI click-move factory not found\n"); bad = 1; }
        else printf("  click move: exe+0x%llX\n", (unsigned long long)(s.uimove - base));
        if (s.charge_err) { printf("  FAIL charge: %s\n", s.charge_err); bad = 1; continue; }
#define R(x) (unsigned long long)((x) - base)
        printf("  charge: scan exe+0x%llX..0x%llX range exe+0x%llX MakeAttack exe+0x%llX AddOrder exe+0x%llX\n"
               "          MakeRef exe+0x%llX SetEntity exe+0x%llX RefInit exe+0x%llX TagCtor exe+0x%llX refvt exe+0x%llX\n"
               "          RefDtor exe+0x%llX TagDtor exe+0x%llX stamp exe+0x%llX clock exe+0x%llX\n",
               R(s.scan_lo), R(s.scan_hi), R(s.range), R(s.mkatk), R(s.addorder), R(s.mkref), R(s.setent), R(s.refinit),
               R(s.tagctor), R(s.refvt), R(s.refdtor), R(s.tagdtor), R(s.ordstamp), R(s.clock));
    }
    cfg_log = 0; init_tag(); g_orig = (match_t)fake_orig;
    static fake_t man, sm_inst, crew, tank, sword, bolter, nade, hvy, enemy;
    memset(&man, 0, sizeof man); man.obj[0x30] = 6;
    fake_vec(man.obj + cfg_enttags, man.tags, "faction_sm", "Melee_Stance", NULL);
    memset(&sm_inst, 0, sizeof sm_inst); sm_inst.obj[0x30] = 6;
    fake_vec(sm_inst.obj + cfg_enttags, sm_inst.tags, "faction_sm", "has_melee_weapon", NULL);
    memset(&crew, 0, sizeof crew); crew.obj[0x30] = 6;
    fake_vec(crew.obj + cfg_enttags, crew.tags, "melee_stance", NULL, NULL);
    *(uint32_t *)(crew.obj + 0x1C0) = 1u << 28;
    memset(&tank, 0, sizeof tank); tank.obj[0x30] = 2;
    fake_vec(tank.obj + cfg_enttags, tank.tags, "melee_stance", NULL, NULL);
    memset(&sword, 0, sizeof sword);
    fake_vec(sword.obj + cfg_ammotags + TAGS_VEC, sword.tags, "melee_vs_human", "pierce", NULL);
    memset(&hvy, 0, sizeof hvy);
    fake_vec(hvy.obj + cfg_ammotags + TAGS_VEC, hvy.tags, "pierce", "melee_vs_all", "melee_vs_human");
    memset(&bolter, 0, sizeof bolter);
    fake_vec(bolter.obj + cfg_ammotags + TAGS_VEC, bolter.tags, "bolt", "small_arms", "pierce");
    memset(&nade, 0, sizeof nade);
    fake_vec(nade.obj + cfg_ammotags + TAGS_VEC, nade.tags, "grenade", "fg", NULL);
    memset(&enemy, 0, sizeof enemy); enemy.obj[0x30] = 6; enemy.obj[0xDA8] = 1;
    struct { fake_t *u, *a; int want; const char *what; } c[] = {
        { &man, &sword, MELEE, "stance + chainsword ammo" },
        { &man, &hvy, MELEE, "stance + heavy melee ammo" },
        { &man, &bolter, REJECT, "stance + bolter ammo" },
        { &man, &nade, REJECT, "stance + grenade" },
        { &sm_inst, &bolter, PASS, "no stance + bolter" },
        { &sm_inst, &sword, PASS, "no stance + chainsword" },
        { &crew, &bolter, PASS, "stance but linked (crew)" },
        { &tank, &bolter, PASS, "non-human tagged" },
    };
    int fails = 0;
    cfg_charge = 0;
    for (size_t i = 0; i < sizeof c / sizeof c[0]; i++) {
        uint8_t ctx[0x20] = { 0 };
        *(void **)ctx = c[i].a->obj; *(void **)(ctx + 8) = c[i].u->obj;
        int got = ms_decide(ctx);
        if (got != c[i].want) { printf("  FAIL %s: got %d\n", c[i].what, got); fails++; }
    }
    {
        uint8_t ctx[0x20] = { 0 };
        *(void **)ctx = bolter.obj; *(void **)(ctx + 8) = man.obj;
        if (ms_hook(ctx) != NULL) { printf("  FAIL hook reject\n"); fails++; }
        *(void **)ctx = sword.obj; g_seen_ready = (void *)7;
        if (ms_hook(ctx) != (void *)0x1234 || g_seen_ready != (void *)1 || ctx[0x18] != 0) { printf("  FAIL hook melee/ready\n"); fails++; }
        *(void **)(ctx + 8) = sm_inst.obj; *(void **)ctx = bolter.obj; g_seen_ready = (void *)7;
        if (ms_hook(ctx) != (void *)0x1234 || g_seen_ready != (void *)0) { printf("  FAIL hook pass\n"); fails++; }
    }
    /* charge path on a fake engine (charge_note / charge_flush directly; the return-address gate is the finder's job) */
    {
        cfg_charge = 1;
        g_tagctor = g_tagdtor = g_refinit = g_refdtor = (vfn1_t)f_nop; g_setent = (setent_t)f_setent;
        g_mkatk = (mkatk_t)f_mkatk; g_addorder = (addord_t)f_add; g_range = (range_t)f_range;
        g_clock = &f_clock; g_ordstamp = &f_stamp; g_refvt = (void *)0x55;
        uint8_t ti[0x40] = { 0 };
        *(void **)(ti + 0x18) = enemy.obj;
        *(float *)(ti + 0x28) = 100.0f;                      /* enemy 100 units away on X */
        uint8_t ctx[0x20] = { 0 };
        *(void **)ctx = sword.obj; *(void **)(ctx + 8) = sm_inst.obj; *(void **)(ctx + 0x10) = ti;
        charge_note(ctx, 0, 0);                              /* not in hand -> nothing */
        int q0 = (int)st_charge_q;
        charge_note(ctx, 1, 0);                              /* in hand, has_melee_weapon -> queued */
        int q1 = (int)st_charge_q;
        charge_flush(NULL);                                  /* same tick -> nothing yet */
        int o0 = f_orders;
        f_clock += 50;
        charge_flush(sm_inst.obj);                           /* the unit itself is scanning -> wait */
        int o1 = f_orders;
        charge_flush(man.obj);                               /* another unit's scan, later tick -> order */
        int o2 = f_orders;
        charge_note(ctx, 1, 0);                              /* same target inside the cooldown -> nothing */
        int q2 = (int)st_charge_q;
        *(float *)(ti + 0x28) = 3.0f;                        /* in reach */
        f_clock += 10000;
        charge_note(ctx, 1, 0);
        int q3 = (int)st_charge_q;
        *(void **)ctx = bolter.obj; *(float *)(ti + 0x28) = 100.0f;
        charge_note(ctx, 1, 0);                              /* ranged ammo -> nothing */
        int q4 = (int)st_charge_q;
        *(void **)ctx = sword.obj; *(void **)(ctx + 8) = man.obj;
        charge_note(ctx, 0, 1);                              /* stance unit, sword on the belt -> queued */
        int q5 = (int)st_charge_q;
        /* always tag counts as stance; beyond MaxDistance and inside MinDistance nothing */
        static fake_t brute; memset(&brute, 0, sizeof brute); brute.obj[0x30] = 6;
        fake_vec(brute.obj + cfg_enttags, brute.tags, "faction_tg", "melee_always", NULL);
        uint8_t c2[0x20] = { 0 };
        *(void **)c2 = sword.obj; *(void **)(c2 + 8) = brute.obj;
        ms_decide(c2);                                       /* his chainsword shows up in a scan */
        *(void **)c2 = bolter.obj;
        int al = ms_decide(c2) == REJECT;
        *(void **)c2 = sword.obj; *(void **)(c2 + 0x10) = ti;
        *(float *)(ti + 0x28) = 2500.0f; charge_note(c2, 0, 1);
        int q6 = (int)st_charge_q;
        *(float *)(ti + 0x28) = 40.0f; charge_note(c2, 0, 1);
        int q7 = (int)st_charge_q;
        *(float *)(ti + 0x28) = 1900.0f; charge_note(c2, 0, 1);
        int q8 = (int)st_charge_q;
        /* with a clock: a stance unit never seen with melee ammo keeps its guns */
        static fake_t lasgunner; memset(&lasgunner, 0, sizeof lasgunner); lasgunner.obj[0x30] = 6;
        fake_vec(lasgunner.obj + cfg_enttags, lasgunner.tags, "melee_stance", NULL, NULL);
        uint8_t c3[0x20] = { 0 };
        *(void **)c3 = bolter.obj; *(void **)(c3 + 8) = lasgunner.obj;
        int lg = ms_decide(c3) == PASS;
        *(void **)c3 = sword.obj; ms_decide(c3);             /* now he shows a melee weapon */
        *(void **)c3 = bolter.obj;
        int lg2 = ms_decide(c3) == REJECT;
        f_clock += SEEN_MS + 1;                              /* melee weapon gone for 20 s */
        int lg3 = ms_decide(c3) == PASS;
        int ok = q0 == 0 && q1 == 1 && o0 == 0 && o1 == 0 && o2 == 1 && f_last_unit == sm_inst.obj && f_last_tgt == enemy.obj
                 && q2 == 1 && q3 == 1 && q4 == 1 && q5 == 2 && al && q6 == 2 && q7 == 2 && q8 == 3 && lg && lg2 && lg3;
        if (!(lg && lg2 && lg3)) printf("  FAIL carried-melee gate %d %d %d\n", lg, lg2, lg3);
        printf("charge: queue %d/%d flush %d/%d/%d cooldown %d reach %d ranged %d stance %d always %d far %d close %d 95m %d -> %s\n",
               q0, q1, o0, o1, o2, q2, q3, q4, q5, al, q6, q7, q8, ok ? "OK" : "FAIL");
        if (!ok) fails++;
    }
    /* shield melee ray: [shield linked to target, target] passes; [wall, target] blocks; [target] passes; tagged item passes */
    {
        static fake_t tgt, shield, wall, kit; static uint8_t link[0x20];
        memset(&tgt, 0, sizeof tgt); memset(&shield, 0, sizeof shield); memset(&wall, 0, sizeof wall); memset(&kit, 0, sizeof kit);
        *(uint32_t *)(shield.obj + 0x1C0) = 1u << 28; *(uint8_t **)(shield.obj + 0x560) = link; *(uint8_t **)(link + 8) = tgt.obj;
        fake_vec(kit.obj + cfg_enttags, kit.tags, "forcefield", NULL, NULL);
        static uint8_t recs[0x48 * 3]; static uint8_t *vec[2];
        g_ray_hits = (uint8_t *)vec; cfg_ray = 1;
#define HITS(a, b2) do { memset(recs, 0, sizeof recs); *(void **)(recs + 0x18) = (a); *(void **)(recs + 0x48 + 0x18) = (b2); vec[0] = recs; vec[1] = recs + 0x48 * ((b2) ? 2 : 1); } while (0)
        HITS(shield.obj, tgt.obj); int r1 = ms_ray_ok(tgt.obj);
        HITS(wall.obj, tgt.obj);   int r2 = ms_ray_ok(tgt.obj);
        HITS(tgt.obj, NULL);       int r3 = ms_ray_ok(tgt.obj);
        HITS(kit.obj, tgt.obj);    int r4 = ms_ray_ok(tgt.obj);
        HITS(shield.obj, NULL);    int r5 = ms_ray_ok(tgt.obj);
        int ok = r1 == 1 && r2 == 0 && r3 == 1 && r4 == 1 && r5 == 0;
        printf("shield ray: shield+target %d wall+target %d target %d tagged+target %d shield-only %d -> %s\n", r1, r2, r3, r4, r5, ok ? "OK" : "FAIL");
        if (!ok) fails++;
    }
    printf("filter: %zu cases + 3 hook cases + charge, %d failed\n", sizeof c / sizeof c[0], fails);
    if (fails) bad = 1;
    printf(bad ? "SOME FAILED\n" : "ALL OK\n");
    return bad;
}
#endif
