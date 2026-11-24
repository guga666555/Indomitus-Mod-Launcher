/* gem_melee integration rules R1-R8 (spec section 6). Log only, except R3's cease-fire veto (pen's weapon stop of a weapon whose
 * item tags hold chase's melee word; cannot trigger with today's data).
 *
 * Link rules (CS-B / CS-4): link calls into a module only through the read-only APIs of core.h 2.1. Every table here is log-only,
 * fixed-size and direct-mapped (1024 slots, slot = core_suspect_slot(unit)), keyed by (entity pointer, entity id read at note time
 * from the live unit, e+0xC0); a slot whose key differs is overwritten, never searched. One SRW lock (a leaf lock: never held while
 * calling anything outside link, logging included) guards the tables; it is taken only inside the note / trace / veto hooks, which
 * the core calls for suspects, front-order changes, traced units and weapon stops only. Every engine name link keeps is COPIED at
 * note time (core_vec_names); stored unit / ammo / target / weapon pointers are only printed, never dereferenced after the note call
 * returns. A flip block is built inside the note call that trips it, on the calling (engine) thread, while that unit is live. */
#include "../core/core.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LK_SLOTS   CORE_SUSPECT_N          /* 1024 */
#define LK_RING    8                       /* front-order changes kept per unit (the block prints them; FlipChanges is clamped to it) */
#define LK_NAME    48                      /* order class name copy */
#define LK_AMMO    160                     /* ammo tag names copy */
#define LK_MUTE_MS 10000u                  /* a unit that tripped is muted for 10 s of sim time */
#define LK_UNIT_BLOCKS 2                   /* flip blocks per unit per battle: one looping unit (OB-5 dread) cannot spend FlipLines alone */
#define LK_CHASE_NOTE "chase approach"     /* the name chase's C13 note passes (mc/melee.c): every re-registration has the same class */
#define LK_ID_OFF  0xC0                    /* entity id (pf:747; core_unit's id field) */
#define LK_TAG     0x60                    /* one engine Tag (mc/melee:31) */
#define LK_TAGNAME 0x40                    /* MSVC std::string inside a Tag (mc/melee:32) */

static const char *const k_mod[CORE_MOD_N] = { "core", "chase", "stance", "pen", "link" };
static const char *mod_name(int m) { return (unsigned)m < CORE_MOD_N ? k_mod[m] : "?"; }

/* ------------------------------------------------------------------------------------------------ settings (step 1) */
static int      cfg_flip = 1, cfg_flip_changes = 5, cfg_flip_lines = 20, cfg_trace = 0, cfg_trace_lines = 2000, cfg_veto = 1;
static int      cfg_cost = 0, cfg_hooktime = 1;
static uint32_t cfg_flip_win = 3000, cfg_back_ms = 1000;            /* cfg_back_ms = [Core] NewBattleBackMs (link's own rule, RS-1) */
static int64_t  g_bias_q16;                                         /* R8: QPC ticks x 65536 an empty CORE_TIME pair reports (MPC-2) */
static char     cfg_trace_tag[64] = "ai_trace";
static char     cfg_mtag[48] = "melee", cfg_mtag2[48] = "veh_melee";   /* [meleechase] melee_tag / vehicle_melee_tag (chase's sizes) */
static size_t   cfg_mtag_len = 5, cfg_mtag2_len = 9;

/* ------------------------------------------------------------------------------------------------ module answers (step 3) */
static int      g_layout;                                      /* chase_layout found: R3 veto available */
static uint32_t g_weap_item, g_tag_beg, g_tag_end;
static uint32_t g_tag_off = 0x4E8, g_name_off = 0xB80;         /* entity tag vector, ammo name tag vector */

/* ------------------------------------------------------------------------------------------------ state */
static SRWLOCK       g_lk = SRWLOCK_INIT;
static int           g_attached;
static volatile int  g_force_suspect, g_force_trace;           /* link_test_force */
static volatile LONG g_notes, g_traces, g_flips;               /* hook calls (notes), trace lines (before the cap), flip blocks logged */
static volatile LONG g_n_front, g_n_pick, g_n_verd;            /* hook calls per kind (link_test_sources) */

typedef struct {
    const void *unit; uint32_t id; int used;
    uint32_t t[LK_RING]; const void *vt[LK_RING]; char nm[LK_RING][LK_NAME]; int n;   /* ring: change k at k % LK_RING */
    uint32_t mute_until; int muted, blocks;                                              /* blocks written this battle */
    int s_has, s_code; uint32_t s_t; const void *s_ammo, *s_tgt; char s_names[LK_AMMO];   /* stance: last non-PASS pick */
    int p_has, p_code, p_tnull; uint32_t p_t, p_tid;                                     /* pen: last A verdict */
    int c_has; uint32_t c_t;                                                             /* chase: last hold */
} lk_unit_t;
static lk_unit_t g_u[LK_SLOTS];

typedef struct { const void *e; uint32_t key; int ans; } lk_tc_t;   /* R5 tag test cache: (entity, sim clock + 1) */
static lk_tc_t g_tc[LK_SLOTS];

typedef struct {
    const void *unit; uint32_t id, now, span; int count, nch, chase, last;   /* chase = chase approach notes; last = the unit's last block */
    uint32_t t[LK_RING]; const void *vt[LK_RING]; char nm[LK_RING][LK_NAME];
    int s_has, s_code; uint32_t s_t; const void *s_ammo, *s_tgt; char s_names[LK_AMMO];
    int p_has, p_code, p_tnull; uint32_t p_t, p_tid;
    int c_has; uint32_t c_t;
} lk_block_t;

/* per battle (link_on_new_battle resets them) */
static int           g_flip_lines_b, g_flip_supp_b, g_flip_capped_b;   /* under g_lk: blocks written, trips not written, FlipLines hit */
static uint32_t      g_last_st, g_last_ch;                     /* under g_lk: last stance / chase front-note clock (link's own rule) */
static volatile LONG g_trace_lines_b, g_r3v_lines_b, g_r3c_lines_b, g_own_lines;
static void         *g_cf_w;                                   /* R3 collision record (log only; the core keeps its own counter) */
static uint32_t      g_cf_t;
static int           g_cf_mod = -1;

static uint32_t rd_id(const void *u) { return *(const uint32_t *)((const uint8_t *)u + LK_ID_OFF); }
static uint32_t slot_of(const void *u) { return core_suspect_slot(u) & (LK_SLOTS - 1); }
static void cpy(char *d, size_t cap, const char *s) { size_t k = 0; if (s) for (; s[k] && k + 1 < cap; k++) d[k] = s[k]; d[k] = 0; }

/* "exe+0x.." inside the exe image, else the absolute value */
static void rva_str(const volatile void *p, char *out, size_t cap)
{
    const uint8_t *a = (const uint8_t *)(uintptr_t)p, *b = core_exe_base();
    uint32_t size = 0;
    if (b && ((const IMAGE_DOS_HEADER *)b)->e_magic == IMAGE_DOS_SIGNATURE) {
        const IMAGE_NT_HEADERS64 *nt = (const IMAGE_NT_HEADERS64 *)(b + ((const IMAGE_DOS_HEADER *)b)->e_lfanew);
        if (nt->Signature == IMAGE_NT_SIGNATURE) size = nt->OptionalHeader.SizeOfImage;
    }
    if (!a) snprintf(out, cap, "NULL");
    else if (size && a >= b && a < b + size) snprintf(out, cap, "exe+0x%llX", (unsigned long long)(a - b));
    else snprintf(out, cap, "0x%llX", (unsigned long long)(uintptr_t)a);
}

/* ------------------------------------------------------------------------------------------------ new battle */
/* under g_lk; returns the number of flip trips the ending battle did not write */
static int lk_reset_locked(void)
{
    int supp = g_flip_supp_b;
    memset(g_u, 0, sizeof g_u);
    memset(g_tc, 0, sizeof g_tc);
    memset(core_suspect, 0, sizeof(core_suspect_t) * CORE_SUSPECT_N);
    g_flip_lines_b = g_flip_supp_b = g_flip_capped_b = 0;
    g_last_st = g_last_ch = 0;
    g_cf_w = NULL; g_cf_t = 0; g_cf_mod = -1;
    return supp;
}
/* outside g_lk, after lk_reset_locked */
static void lk_reset_after(int supp)
{
    InterlockedExchange(&g_trace_lines_b, 0);
    InterlockedExchange(&g_r3v_lines_b, 0);
    InterlockedExchange(&g_r3c_lines_b, 0);
    if (supp > 0)
        core_log(CORE_MOD_LINK, "flip: %d more trip%s in the last battle were not written (FlipLines=%d per battle, %d per unit)", supp,
                 supp == 1 ? "" : "s", cfg_flip_lines, LK_UNIT_BLOCKS);
}
static void lk_reset_battle(void)
{
    AcquireSRWLockExclusive(&g_lk);
    int supp = lk_reset_locked();
    ReleaseSRWLockExclusive(&g_lk);
    lk_reset_after(supp);
}

/* core epoch listener: runs on the detecting thread (maybe inside pen's g_lock or stance's hook): no module call, only link's lock
 * (and the core's log line, as the core's own "--- new battle" line right before it) */
static void link_on_new_battle(uint32_t epoch, uint32_t now) { (void)epoch; (void)now; lk_reset_battle(); }

/* ------------------------------------------------------------------------------------------------ R4 flip detector */
/* changes inside the window, newest first (the ring is in time order); *oldest = the oldest one counted */
static int win_count(const lk_unit_t *s, uint32_t now, uint32_t *oldest)
{
    int k = s->n < LK_RING ? s->n : LK_RING, c = 0;
    uint32_t o = now;
    for (int i = 0; i < k; i++) {
        uint32_t t = s->t[(s->n - 1 - i) % LK_RING];
        if (t > now || now - t > cfg_flip_win) break;
        c++; o = t;
    }
    if (oldest) *oldest = o;
    return c;
}

static void block_fill(lk_block_t *b, const lk_unit_t *s, uint32_t now, int count, uint32_t oldest)
{
    memset(b, 0, sizeof *b);
    b->unit = s->unit; b->id = s->id; b->now = now; b->span = now - oldest; b->count = count;
    int k = s->n < LK_RING ? s->n : LK_RING;
    b->nch = k;
    for (int i = 0; i < k; i++) {                                     /* oldest first */
        int r = (s->n - k + i) % LK_RING;
        b->t[i] = s->t[r]; b->vt[i] = s->vt[r]; memcpy(b->nm[i], s->nm[r], LK_NAME);
    }
    b->s_has = s->s_has; b->s_code = s->s_code; b->s_t = s->s_t; b->s_ammo = s->s_ammo; b->s_tgt = s->s_tgt;
    memcpy(b->s_names, s->s_names, LK_AMMO);
    b->p_has = s->p_has; b->p_code = s->p_code; b->p_tnull = s->p_tnull; b->p_t = s->p_t; b->p_tid = s->p_tid;
    b->c_has = s->c_has; b->c_t = s->c_t;
}

static const char *stance_code_name(int code)
{
    switch (code & 0xFF) {
    case CORE_PICK_REJECT: return (code & CORE_PICK_NOMELEE) ? "no-melee REJECT" : "REJECT";
    case CORE_PICK_MELEE:  return "MELEE";
    default:               return "PASS";
    }
}

/* the block is written outside the lock; the header reads the live unit (we are inside its note call) unless replaying */
static void block_write(const lk_block_t *b, int replay)
{
    char who[96];
    if (replay) snprintf(who, sizeof who, "replay");
    else {
        core_unit_t cu; core_unit(b->unit, &cu);
        snprintf(who, sizeof who, "%s, owner %u, id 0x%X%s", cu.human ? "human" : cu.vehicle ? "vehicle" : "actor", (unsigned)cu.owner,
                 cu.id, cu.traced ? ", traced" : "");
    }
    core_log(CORE_MOD_LINK, "flip: [%u ms] unit %p (%s) %s %d times in %u ms (FlipChanges=%d, FlipWindowMs=%u):",
             b->now, b->unit, who, b->chase ? "chase approach registered" : "front order changed", b->count, b->span, cfg_flip_changes, cfg_flip_win);
    for (int i = 0; i < b->nch; i++) core_log(CORE_MOD_LINK, "  [%u ms] %s (vt %p)", b->t[i], b->nm[i], b->vt[i]);
    char tg[32];
    if (b->s_tgt) snprintf(tg, sizeof tg, "%p", b->s_tgt); else snprintf(tg, sizeof tg, "none");
    if (!b->s_has) core_log(CORE_MOD_LINK, "  stance: none");
    else if (!b->s_ammo) core_log(CORE_MOD_LINK, "  stance: last %s at %u ms: ammo none vs %s", stance_code_name(b->s_code), b->s_t, tg);
    else core_log(CORE_MOD_LINK, "  stance: last %s at %u ms: ammo \"%s\" vs %s", stance_code_name(b->s_code), b->s_t, b->s_names, tg);
    if (!b->p_has) core_log(CORE_MOD_LINK, "  pen: none");
    else if (b->p_tnull) core_log(CORE_MOD_LINK, "  pen: last %s vs target none at %u ms", b->p_code == CORE_PEN_ALLOW ? "ALLOW" : "REJECT", b->p_t);
    else core_log(CORE_MOD_LINK, "  pen: last %s vs target 0x%X at %u ms", b->p_code == CORE_PEN_ALLOW ? "ALLOW" : "REJECT", b->p_tid, b->p_t);
    if (!b->c_has) core_log(CORE_MOD_LINK, "  chase: none");
    else core_log(CORE_MOD_LINK, "  chase: last hold at %u ms", b->c_t);
    if (b->last) core_log(CORE_MOD_LINK, "  (no more flip blocks for this unit in this battle: %d per unit)", LK_UNIT_BLOCKS);
    InterlockedIncrement(&g_flips);
}

/* one front-order change; returns 1 when it wrote a flip block */
static int r4_front(const void *u, uint32_t id, const void *vt, const char *name, uint32_t now, int replay)
{
    lk_block_t lb;                             /* filled under g_lk, written right after the lock is released */
    int trip = 0, capped = 0, own = -1;
    const int chase = name && !strcmp(name, LK_CHASE_NOTE);
    uint32_t sl = slot_of(u), was = 0;
    AcquireSRWLockExclusive(&g_lk);
    /* link's own new-battle rule (RS-1): the note's clock (stance's / chase's own) stepped back by [Core] NewBattleBackMs. Covers a
     * session without a core clock and one where no module polls the core detector (chase alone); otherwise the core's listener
     * has already reset everything (and these two clocks) at the restart. Log only: no module is told anything. */
    uint32_t *last = chase ? &g_last_ch : &g_last_st;
    if (*last && now < *last && *last - now >= cfg_back_ms) { was = *last; own = lk_reset_locked(); }
    *last = now ? now : 1;
    lk_unit_t *s = &g_u[sl];
    if (!s->used || s->unit != u || s->id != id) { memset(s, 0, sizeof *s); s->used = 1; s->unit = u; s->id = id; }
    /* a stance note that repeats the class of this unit's previous change is no change: stance's unit slot was taken over by another
     * unit and handed back empty (unit_of, ms:330-348), so its ovt test saw NULL -> the same class (MPC-4). Chase notes always carry
     * the approach order's class and every one counts (the OB-5 loop, Q7). */
    if (!chase && s->n > 0 && s->vt[(s->n - 1) % LK_RING] == vt) {
        ReleaseSRWLockExclusive(&g_lk);
        if (own >= 0) {
            lk_reset_after(own);
            if (InterlockedIncrement(&g_own_lines) <= 50)
                core_log(CORE_MOD_LINK, "flip: new battle on the stance clock (%u -> %u ms, link's own rule): flip tables cleared", was, now);
        }
        return 0;
    }
    int r = s->n % LK_RING;
    s->t[r] = now; s->vt[r] = vt; cpy(s->nm[r], LK_NAME, name);
    if (++s->n >= (1 << 30)) s->n = LK_RING + s->n % LK_RING;
    uint32_t oldest = now;
    int c = win_count(s, now, &oldest);
    if (c >= 2) { core_suspect[sl].unit = u; core_suspect[sl].id = id; core_suspect[sl].until = now + cfg_flip_win; }
    if (s->muted && (int32_t)(now - s->mute_until) >= 0) s->muted = 0;
    if (c >= cfg_flip_changes && !s->muted) {
        s->muted = 1; s->mute_until = now + LK_MUTE_MS;
        if (s->blocks >= LK_UNIT_BLOCKS) g_flip_supp_b++;                               /* this unit had its blocks: counted only */
        else if (g_flip_lines_b >= cfg_flip_lines) { g_flip_supp_b++; if (!g_flip_capped_b) { g_flip_capped_b = 1; capped = 1; } }
        else {
            g_flip_lines_b++; s->blocks++; trip = 1;
            block_fill(&lb, s, now, c, oldest);
            lb.chase = chase; lb.last = s->blocks >= LK_UNIT_BLOCKS;
        }
    }
    ReleaseSRWLockExclusive(&g_lk);
    if (own >= 0) {
        lk_reset_after(own);
        if (InterlockedIncrement(&g_own_lines) <= 50)
            core_log(CORE_MOD_LINK, "flip: new battle on the %s clock (%u -> %u ms, link's own rule): flip tables cleared", chase ? "chase" : "stance", was, now);
    }
    if (trip) block_write(&lb, replay);
    if (capped)
        core_log(CORE_MOD_LINK, "flip: FlipLines=%d reached for this battle: later trips are counted, not written (the count comes at the next "
                 "battle); no block from here on does not mean no flip", cfg_flip_lines);
    return trip;
}

/* a suspect: the core's slot holds this unit with this id and the window has not run out (or link_test_force) */
static int suspect_ok(const void *u, uint32_t id, uint32_t sl, uint32_t now)
{
    if (g_force_suspect) return 1;
    const core_suspect_t *cs = &core_suspect[sl];
    return cs->unit == u && cs->id == id && (int32_t)(cs->until - now) >= 0;
}

/* the unit's slot, taken over for this key only under link_test_force (a real suspect always has one) */
static lk_unit_t *slot_for(const void *u, uint32_t id, uint32_t sl)
{
    lk_unit_t *s = &g_u[sl];
    if (s->used && s->unit == u && s->id == id) return s;
    if (!g_force_suspect) return NULL;
    memset(s, 0, sizeof *s); s->used = 1; s->unit = u; s->id = id;
    return s;
}

static void hk_note_front(const void *unit, const void *order_vt, const char *name, uint32_t now)
{
    InterlockedIncrement(&g_notes); InterlockedIncrement(&g_n_front);
    if (!unit || !cfg_flip) return;
    r4_front(unit, rd_id(unit), order_vt, name ? name : (order_vt ? "?" : "none"), now, 0);
}

static void hk_note_pick(int mod, const uint8_t *ctx, int code, uint32_t now)
{
    InterlockedIncrement(&g_notes); InterlockedIncrement(&g_n_pick);
    if (!ctx) return;
    const void *u = *(const void *const *)(ctx + 8);
    if (!u) return;
    uint32_t id = rd_id(u), sl = slot_of(u);
    const uint8_t *ammo = *(const uint8_t *const *)ctx, *ti = *(const uint8_t *const *)(ctx + 0x10);
    const void *tgt = ti ? *(const void *const *)(ti + 0x18) : NULL;
    int ok, need = 1;
    AcquireSRWLockShared(&g_lk);
    ok = suspect_ok(u, id, sl, now);
    if (ok) { const lk_unit_t *s = &g_u[sl]; need = !(s->used && s->unit == u && s->id == id && s->s_has && s->s_ammo == ammo && s->s_code == code); }
    ReleaseSRWLockShared(&g_lk);
    if (!ok) return;
    char names[LK_AMMO]; names[0] = 0;
    if (need && ammo) core_vec_names(ammo + g_name_off, names, sizeof names);   /* copy only when (ammo, code) changed */
    AcquireSRWLockExclusive(&g_lk);
    lk_unit_t *s = slot_for(u, id, sl);
    if (s) {
        if (!s->s_has || s->s_ammo != ammo || s->s_code != code) { s->s_ammo = ammo; s->s_code = code; memcpy(s->s_names, names, LK_AMMO); }
        s->s_has = 1; s->s_t = now; s->s_tgt = tgt;
    }
    ReleaseSRWLockExclusive(&g_lk);
}

static void hk_note_verdict(int mod, const void *unit, const void *target, int code, uint32_t now)
{
    InterlockedIncrement(&g_notes); InterlockedIncrement(&g_n_verd);
    if (!unit) return;
    uint32_t id = rd_id(unit), sl = slot_of(unit), tid = target ? rd_id(target) : 0;
    AcquireSRWLockExclusive(&g_lk);
    lk_unit_t *s = suspect_ok(unit, id, sl, now) ? slot_for(unit, id, sl) : NULL;
    if (s && mod == CORE_MOD_PEN) { s->p_has = 1; s->p_code = code; s->p_t = now; s->p_tid = tid; s->p_tnull = target == NULL; }
    else if (s && mod == CORE_MOD_CHASE) { s->c_has = 1; s->c_t = now; }
    ReleaseSRWLockExclusive(&g_lk);
}

/* ------------------------------------------------------------------------------------------------ R5 trace */
static int hk_trace_unit(const void *u)
{
    if (!u || (!cfg_trace && !g_force_trace)) return 0;
    uint32_t key = core_now() + 1, sl = slot_of(u);
    int hit = 0, ans = 0, cache = core_clock_ptr() != NULL;        /* no core clock: the key never changes, read every time (RS-1) */
    if (cache) {
        AcquireSRWLockShared(&g_lk);
        if (g_tc[sl].e == u && g_tc[sl].key == key) { hit = 1; ans = g_tc[sl].ans; }
        ReleaseSRWLockShared(&g_lk);
    }
    if (!hit) {
        ans = core_vec_has_ci((const uint8_t *)u + g_tag_off, cfg_trace_tag) == 1;   /* stance's vec_has semantics, ms:123-137 */
        if (cache) {
            AcquireSRWLockExclusive(&g_lk);
            g_tc[sl].e = u; g_tc[sl].key = key; g_tc[sl].ans = ans;
            ReleaseSRWLockExclusive(&g_lk);
        }
    }
    return g_force_trace ? 1 : ans;
}

static void trace_write(int mod, const void *unit, uint32_t now, const char *text)
{
    InterlockedIncrement(&g_traces);
    LONG k = InterlockedIncrement(&g_trace_lines_b);
    if (k <= (LONG)cfg_trace_lines) core_log(CORE_MOD_LINK, "trace: [%u ms] %s unit %p: %s", now, mod_name(mod), unit, text);
    else if (k == (LONG)cfg_trace_lines + 1)
        core_log(CORE_MOD_LINK, "trace: TraceLines=%d reached for this battle, no more trace lines until the next one", cfg_trace_lines);
}

static void hk_trace_pick(int mod, const uint8_t *ctx, int code, uint32_t now)
{
    if (!ctx) return;
    const void *u = *(const void *const *)(ctx + 8);
    if (!hk_trace_unit(u)) return;
    const uint8_t *ammo = *(const uint8_t *const *)ctx, *ti = *(const uint8_t *const *)(ctx + 0x10);
    const void *tgt = ti ? *(const void *const *)(ti + 0x18) : NULL;
    char names[LK_AMMO], am[LK_AMMO + 4], tg[32], txt[320];
    if (ammo) { core_vec_names(ammo + g_name_off, names, sizeof names); snprintf(am, sizeof am, "\"%s\"", names); }
    else snprintf(am, sizeof am, "none");
    if (tgt) snprintf(tg, sizeof tg, "%p", tgt); else snprintf(tg, sizeof tg, "none");
    const char *what = code == CORE_PICK_DOWNED ? "downed target: no weapon" : code == CORE_PICK_FAST ? "fast pass: engine's choice"
                     : (code & 0xFF) == CORE_PICK_PASS ? "PASS: engine's choice" : stance_code_name(code);
    snprintf(txt, sizeof txt, "pick %s: ammo %s vs target %s", what, am, tg);
    trace_write(mod, u, now, txt);
}

static void hk_trace_vline(int mod, const void *unit, const char *fmt, va_list ap)
{
    char txt[512];
    vsnprintf(txt, sizeof txt, fmt ? fmt : "", ap);
    trace_write(mod, unit, core_now(), txt);
}

/* ------------------------------------------------------------------------------------------------ R3 cease-fire veto */
/* chase's weapon_is_melee by name (mc/melee:180, 189-193, 202-205), no interned ids: chase's id-trust state never moves */
static const char *weapon_melee_word(void *w)
{
    const uint8_t *item = *(const uint8_t *const *)((const uint8_t *)w + g_weap_item);
    if (!item) return NULL;
    const uint8_t *beg = *(const uint8_t *const *)(item + g_tag_beg), *end = *(const uint8_t *const *)(item + g_tag_end);
    if (!beg || end < beg || (size_t)(end - beg) > LK_TAG * 256) return NULL;
    for (const uint8_t *t = beg; t + LK_TAG <= end; t += LK_TAG) {
        const uint8_t *s = t + LK_TAGNAME;
        size_t size = *(const size_t *)(s + 0x10), cap = *(const size_t *)(s + 0x18);
        const char *cs = cap > 15 ? *(const char *const *)s : (const char *)s;
        if (size == cfg_mtag_len && !_strnicmp(cs, cfg_mtag, cfg_mtag_len)) return cfg_mtag;
        if (cfg_mtag2_len && size == cfg_mtag2_len && !_strnicmp(cs, cfg_mtag2, cfg_mtag2_len)) return cfg_mtag2;
    }
    return NULL;
}

static int hk_veto(int mod, void *weapon)
{
    if (!weapon) return 0;
    uint32_t now = core_now();
    if (mod == CORE_MOD_PEN && cfg_veto && g_layout) {
        const char *word = weapon_melee_word(weapon);
        if (word) {
            if (InterlockedIncrement(&g_r3v_lines_b) <= 20)   /* pen writes its X line / trace before it asks (pf:1555-1562): say what happened */
                core_log(CORE_MOD_LINK, "R3: pen stop of melee weapon %p (item tag %s) refused: the weapon was NOT stopped (pen's X line or trace "
                         "for this pick says \"weapon stopped\")", weapon, word);
            return 1;
        }
    }
    int coll = 0;
    AcquireSRWLockExclusive(&g_lk);
    if (g_cf_w == weapon && g_cf_t == now && g_cf_mod != mod) coll = 1;
    g_cf_w = weapon; g_cf_t = now; g_cf_mod = mod;
    ReleaseSRWLockExclusive(&g_lk);
    if (coll && InterlockedIncrement(&g_r3c_lines_b) <= 20)
        core_log(CORE_MOD_LINK, "R3: weapon %p stopped by chase and pen in the same sim ms", weapon);
    return 0;
}

/* ------------------------------------------------------------------------------------------------ R8 cost line */
static long long q_us(int64_t q, int64_t f) { return (long long)((__int128)q * 1000000 / f); }
static long long q_ns(int64_t q, int64_t f, long n) { return n ? (long long)((__int128)q * 1000000000 / f / n) : 0; }

/* bias_q16 = what one empty CORE_TIME_BEGIN / END pair reports (QPC ticks x 65536, measured at attach): it is subtracted from the
 * pen A total once per call, and that time stays in "engine alone" (MPC-2: without it pen A read about one QPC latency high) */
static int cost_text(char *out, int cap, uint32_t now, int64_t hq, long hn, int64_t oq, int64_t aq, long an, int64_t f, int64_t bias_q16)
{
    if (f <= 0) f = 1;
    if (bias_q16 < 0) bias_q16 = 0;
    int64_t aqc = aq - (int64_t)(((__int128)an * bias_q16) >> 16);
    if (aqc < 0) aqc = 0;
    long long bns = (long long)((__int128)bias_q16 * 1000000000 / f / 65536);
    return snprintf(out, (size_t)cap, "cost: [%u ms] stance own %lld us over %ld calls (%lld ns/call) | engine MatchAmmo %lld us incl. pen A %lld us "
                    "over %ld calls (%lld ns/call, timer bias %lld ns/call removed) | engine alone %lld us",
                    now, q_us(hq, f), hn, q_ns(hq, f, hn), q_us(oq, f), q_us(aqc, f), an, q_ns(aqc, f, an), bns, q_us(oq - aqc, f));
}

/* core thread, every ~100 ms: one line every 5 s wall while the counters change (max 400); counters only, no unit pointers */
static void link_bg_step(void)
{
    static DWORD last; static int lines; static int64_t lh, lo, la; static long lhn, lan;
    if (!core_timing_on || lines >= 400) return;
    DWORD t = GetTickCount();
    if (last && t - last < 5000) return;
    last = t ? t : 1;
    int64_t hq = 0, oq = 0, pq = 0, aq = 0; long hn = 0, an = 0;
    stance_cost_read(&hq, &oq, &hn, &pq);
    core_time_read(CORE_T_PEN_A, &aq, &an);
    if (hq == lh && oq == lo && aq == la && hn == lhn && an == lan) return;
    lh = hq; lo = oq; la = aq; lhn = hn; lan = an; lines++;
    LARGE_INTEGER f; QueryPerformanceFrequency(&f);
    char l[512];
    cost_text(l, sizeof l, core_now(), hq, hn, oq, aq, an, f.QuadPart, g_bias_q16);
    core_log(CORE_MOD_LINK, "%s", l);
}

/* ------------------------------------------------------------------------------------------------ R1 owner words */
static void ini_s(const char *path, const char *sec, const char *key, const char *def, char *out, int cap)
{
    if (path && path[0]) GetPrivateProfileStringA(sec, key, def, out, (DWORD)cap, path);
    else snprintf(out, (size_t)cap, "%s", def);
}

static int words(const char *src, char w[][64], int max)       /* comma / space / tab separated, as stance's split_list and pen's strtok */
{
    int n = 0; const char *p = src;
    while (*p && n < max) {
        while (*p == ' ' || *p == ',' || *p == '\t') p++;
        int k = 0;
        while (*p && *p != ',' && *p != ' ' && *p != '\t') { if (k < 63) w[n][k++] = *p; p++; }
        w[n][k] = 0;
        if (k) n++;
    }
    return n;
}
static int in_list(const char *s, char w[][64], int n) { for (int i = 0; i < n; i++) if (!_stricmp(s, w[i])) return 1; return 0; }
static void join(char *out, size_t cap, char w[][64], int n)
{
    out[0] = 0;
    for (int i = 0; i < n; i++) { size_t l = strlen(out); snprintf(out + l, cap - l, "%s%s", i ? "," : "", w[i]); }
}

int link_word_check(const char *ini_path, char *out, int cap)
{
    char ex_s[1024], vt_s[1024], ma_s[1024], mt[48], mt2[48];
    ini_s(ini_path, "PenFilter", "ExemptTags", "melee_light,melee_heavy,melee", ex_s, sizeof ex_s);     /* pf:1715 default */
    ini_s(ini_path, "Vehicles", "VehicleMeleeTags", "melee_light,melee_heavy", vt_s, sizeof vt_s);    /* ms:3085 default */
    ini_s(ini_path, "MeleeStance", "MeleeAmmoTags",
          "melee_light,melee_heavy,medium_inf_melee_target,medium_veh_melee_target,heavy_veh_melee_target", ma_s, sizeof ma_s);   /* ms:3000 */
    ini_s(ini_path, "meleechase", "melee_tag", "melee", mt, sizeof mt);                                /* mc/main:35 */
    ini_s(ini_path, "meleechase", "vehicle_melee_tag", "veh_melee", mt2, sizeof mt2);                  /* mc/main:39 */
    static char ex[32][64], vw[32][64], ma[32][64], cw[2][64];
    int nex = words(ex_s, ex, 32), nvw = words(vt_s, vw, 32), nma = words(ma_s, ma, 32), ncw = 0;
    if (mt[0]) cpy(cw[ncw++], 64, mt);
    if (mt2[0]) cpy(cw[ncw++], 64, mt2);
    int byname = 0;
    for (int i = 0; i < nma; i++) if (in_list(ma[i], ex, nex)) byname++;
    char a[1200], b[1200], c[200];
    join(a, sizeof a, ex, nex); join(b, sizeof b, ma, nma); join(c, sizeof c, cw, ncw);
    char line[4096];
    int k = snprintf(line, sizeof line, "R1 owner words: pen exempts {%s}; stance melee ammo {%s} (%d exempt by name; the others must also carry an "
                     "exempt word in the mod data); chase melee weapon tags {%s} (weapon item tags, pen reads ammo tags only)", a, b, byname, c);
    int warn = 0;
    for (int i = 0; i < nvw; i++)
        if (!in_list(vw[i], ex, nex) && k < (int)sizeof line) {
            warn++;
            k += snprintf(line + k, sizeof line - (size_t)k, " | WARNING: [Vehicles] VehicleMeleeTags word \"%s\" is not in [PenFilter] ExemptTags "
                          "(pen may filter stance's vehicle melee ammo)", vw[i]);
        }
    if (mt[0] && !in_list(mt, ex, nex) && k < (int)sizeof line) {
        warn++;
        k += snprintf(line + k, sizeof line - (size_t)k, " | WARNING: [meleechase] melee_tag \"%s\" is not in [PenFilter] ExemptTags", mt);
    }
    if (out && cap > 0) snprintf(out, (size_t)cap, "%s", line);
    return warn;
}

/* ------------------------------------------------------------------------------------------------ attach (spec 6, steps 1-8) */
static const core_link_hooks_t k_hooks = {
    hk_note_front, hk_note_pick, hk_note_verdict, hk_trace_pick, hk_trace_unit, hk_trace_vline, hk_veto
};

static void log_cross_checks(int sc, int ss, int sp)
{
    char a[48], b[48];
    /* clock check: every module that is on must use the core's clock (core_epoch_bind detaches the others) */
    volatile uint32_t *cc = core_clock_ptr();
    if (!cc) core_log(CORE_MOD_LINK, "clock check: the core found no sim clock (every module keeps its own new-battle rule)");
    else {
        char l[400]; int k, same = 1;
        rva_str(cc, a, sizeof a);
        k = snprintf(l, sizeof l, "clock check: core %s", a);
        const void *mc[3] = { ss ? stance_clock_ptr() : NULL, sc ? chase_clock_ptr() : NULL, sp ? pen_clock_ptr() : NULL };
        const int on[3] = { ss, sc, sp };
        const char *nm[3] = { "stance", "chase", "pen" };
        for (int i = 0; i < 3; i++) {
            if (!on[i]) continue;
            rva_str(mc[i], b, sizeof b);
            k += snprintf(l + k, sizeof l - (size_t)k, ", %s %s", nm[i], b);
            if (mc[i] != (const void *)cc) same = 0;
        }
        if (!ss && !sc && !sp) core_log(CORE_MOD_LINK, "%s (no module is on)", l);
        else if (same) core_log(CORE_MOD_LINK, "%s: equal", l);
        else core_log(CORE_MOD_LINK, "%s: WARNING: not all equal (a module on another clock keeps only its own new-battle rule)", l);
    }
    /* cease-fire: chase's ClearTarget and pen's weapon stop are one engine function (R3) */
    if (sc && sp) {
        void *cf = chase_ceasefire_fn(), *ps = pen_stop_fn();
        rva_str(cf, a, sizeof a); rva_str(ps, b, sizeof b);
        if (cf && cf == ps) core_log(CORE_MOD_LINK, "cease-fire: chase clear target %s = pen weapon stop %s: one engine function, both stop through core_ceasefire", a, b);
        else core_log(CORE_MOD_LINK, "cease-fire: WARNING: chase clear target %s, pen weapon stop %s: not the same function", a, b);
    } else core_log(CORE_MOD_LINK, "cease-fire: chase %s, pen %s: no cross-check (both modules are needed)", sc ? "on" : "not on", sp ? "on" : "not on");
    /* R2: stance's H1 is pen's MatchAmmo, pen A1 / A2 inside it, pen W in another function (the order of one pick, spec 6) */
    if (ss && sp) {
        uint8_t *h1 = NULL, *a1 = NULL, *a2 = NULL, *w = NULL;
        for (int i = 0; i < core_patch_count(); i++) {
            core_patch_t e;
            if (!core_patch_get(i, &e) || e.state != CORE_PATCH_WRITTEN || !e.name) continue;
            if (e.mod == CORE_MOD_STANCE && !strcmp(e.name, "H1 MatchAmmo prologue")) h1 = e.addr;
            else if (e.mod == CORE_MOD_PEN && !strcmp(e.name, "A1 volume tag test call")) a1 = e.addr;
            else if (e.mod == CORE_MOD_PEN && !strcmp(e.name, "A2 no-volume row test")) a2 = e.addr;
            else if (e.mod == CORE_MOD_PEN && !strcmp(e.name, "W ctx builder prologue")) w = e.addr;
        }
        uint8_t *ma = (uint8_t *)pen_matchammo_fn();
        int in1 = h1 && a1 >= h1 && a1 < h1 + 0x600, in2 = h1 && a2 >= h1 && a2 < h1 + 0x600, wout = h1 && w && (w < h1 || w >= h1 + 0x600);
        rva_str(h1, a, sizeof a); rva_str(ma, b, sizeof b);
        if (h1 && h1 == ma && in1 && in2 && wout)
            core_log(CORE_MOD_LINK, "R2 one pick: stance H1 %s = pen MatchAmmo %s, pen A1 +0x%llX / A2 +0x%llX inside it, pen W in the ctx builder: pen W, "
                     "stance, engine MatchAmmo with pen A", a, b, (unsigned long long)(a1 - h1), (unsigned long long)(a2 - h1));
        else core_log(CORE_MOD_LINK, "R2 one pick: WARNING: stance H1 %s, pen MatchAmmo %s, A1 %s, A2 %s, W %s (expected: H1 = MatchAmmo, A1 / A2 inside, W "
                      "outside)", a, b, in1 ? "inside" : "not inside", in2 ? "inside" : "not inside", wout ? "outside" : "not outside");
    }
}

int link_attach(int chase_on, int stance_on, int pen_on)
{
    if (g_attached) return 1;
    g_attached = 1;
    /* (1) settings: [Link] and the word keys */
    cfg_flip = core_ini_int("Link", "Flip", 1);
    cfg_flip_changes = core_ini_int("Link", "FlipChanges", 5);
    if (cfg_flip_changes < 2) cfg_flip_changes = 2;
    if (cfg_flip_changes > LK_RING) cfg_flip_changes = LK_RING;
    int win = core_ini_int("Link", "FlipWindowMs", 3000);
    cfg_flip_win = (uint32_t)(win < 1 ? 1 : win > 600000 ? 600000 : win);
    cfg_flip_lines = core_ini_int("Link", "FlipLines", 20);
    if (cfg_flip_lines < 0) cfg_flip_lines = 0;
    int back = core_ini_int("Core", "NewBattleBackMs", 1000);                       /* the core's key and clamp (core_init) */
    cfg_back_ms = (uint32_t)(back < 1 ? 1 : back > 60000 ? 60000 : back);
    cfg_trace = core_ini_int("Link", "Trace", 0);
    core_ini_str("Link", "TraceTag", "ai_trace", cfg_trace_tag, sizeof cfg_trace_tag);
    cfg_trace_lines = core_ini_int("Link", "TraceLines", 2000);
    if (cfg_trace_lines < 0) cfg_trace_lines = 0;
    cfg_veto = core_ini_int("Link", "CeasefireVeto", 1);
    cfg_cost = core_ini_int("Link", "CostLine", 0);
    cfg_hooktime = core_ini_int("MeleeStance", "HookTiming", 1);
    core_ini_str("meleechase", "melee_tag", "melee", cfg_mtag, sizeof cfg_mtag);
    core_ini_str("meleechase", "vehicle_melee_tag", "veh_melee", cfg_mtag2, sizeof cfg_mtag2);
    cfg_mtag_len = strlen(cfg_mtag); cfg_mtag2_len = strlen(cfg_mtag2);
    /* (2) tables: statics (zero); core_suspect[] too */
    lk_reset_battle();
    /* (3) module answers, once */
    g_layout = chase_layout(&g_weap_item, &g_tag_beg, &g_tag_end);
    uint32_t co = chase_enttags_off();
    g_tag_off = co ? co : stance_enttags_off();
    g_name_off = stance_name_vec_off();
    /* R8 timer bias (only for a cost run): the mean of what 4096 empty CORE_TIME pairs report, the same two reads as P5's */
    if (cfg_cost && cfg_hooktime) {
        int64_t s = 0;
        for (int i = 0; i < 4096; i++) { int64_t a = core_qpc(); s += core_qpc() - a; }
        g_bias_q16 = s * 65536 / 4096;
    }
    /* (4) hooks, (5) new-battle listener and the cost step, (6) barrier, (7) the switches last */
    core_set_link_hooks(&k_hooks);
    core_epoch_listen(link_on_new_battle);
    core_bg_register(CORE_MOD_LINK, link_bg_step);
    MemoryBarrier();
    core_trace_on = cfg_trace;
    core_timing_on = cfg_cost && cfg_hooktime;
    core_flip_on = cfg_flip;
    /* (8) R1, R6 and the cross-checks */
    char l[4096];
    link_word_check(core_ini_path(), l, sizeof l);
    core_log(CORE_MOD_LINK, "%s", l);
    char pmax[64];
    core_ini_str("Charge", "PlayerMaxDistance", "1000", pmax, sizeof pmax);
    core_log(CORE_MOD_LINK, "R6 player policy: direct control: stance skips (SkipDirectControl=%d; IsManualControlled with the 0.7.32 component guard, or "
             "the local manual-control manager), pen skips (SkipDirectControl=%d; property or DD8 bit, no manager), chase does not check | human "
             "player's units: stance AutoDraw PlayerUnits=%d, Charge PlayerUnits=%d, PlayerMaxDistance=%s; pen PlayerUnits=%d; chase acts on them",
             core_ini_int("MeleeStance", "SkipDirectControl", 1), core_ini_int("PenFilter", "SkipDirectControl", 1),
             core_ini_int("AutoDraw", "PlayerUnits", 0), core_ini_int("Charge", "PlayerUnits", 1), pmax, core_ini_int("PenFilter", "PlayerUnits", 1));
    log_cross_checks(chase_on, stance_on, pen_on);
    if (!cfg_veto) core_log(CORE_MOD_LINK, "R3 cease-fire veto: not used (CeasefireVeto=0)");
    else if (g_layout) core_log(CORE_MOD_LINK, "R3 cease-fire veto: pen's weapon stop is refused for a weapon whose item tags hold \"%s\"%s%s%s (weapon +0x%X "
                                "-> item, tags +0x%X..+0x%X); chase's hold is never refused", cfg_mtag, cfg_mtag2_len ? " or \"" : "",
                                cfg_mtag2_len ? cfg_mtag2 : "", cfg_mtag2_len ? "\"" : "", g_weap_item, g_tag_beg, g_tag_end);
    else core_log(CORE_MOD_LINK, "R3 cease-fire veto: unavailable (chase found no weapon layout), pen's weapon stops go through unchanged");
    /* R4's inputs (RQ-4): stance's order poll (human units on the poll list, sampled every PollMs; not the human player's own units
     * outside the stance with [AutoDraw] PlayerUnits=0, nor linked / SkipTags / directly controlled units) and chase's registrations.
     * Whether the poll can run is stance's own early-return test (S21, review N2): its switches, auto-draw on, an attack-order vtable */
    int pst = stance_on ? stance_order_poll_state() : 0;
    const char *poll = !stance_on ? "OFF (stance off: no human unit is seen)"
                     : pst == 1 ? "on"
                     : pst == 0 ? "OFF ([AutoDraw] OrderPoll=0: no human unit is seen)"
                     : pst == -1 ? "OFF (stance's auto-draw is off: [AutoDraw] Enabled=0 or its charge finders failed; no human unit is seen)"
                     : "OFF (stance found neither attack-order vtable, eAttack / eAttackBasic: no human unit is seen)";
    char r8[160];
    LARGE_INTEGER qf; QueryPerformanceFrequency(&qf);
    if (core_timing_on) snprintf(r8, sizeof r8, "every 5 s while it changes (CostLine=1; timer bias %lld ns per pen A call removed)",
                                 (long long)((__int128)g_bias_q16 * 1000000000 / 65536 / (qf.QuadPart > 0 ? qf.QuadPart : 1)));
    else snprintf(r8, sizeof r8, "%s", cfg_cost ? "not written ([MeleeStance] HookTiming=0)" : "not written (CostLine=0)");
    core_log(CORE_MOD_LINK, "R4 flip detector: %s (FlipChanges=%d, FlipWindowMs=%u, FlipLines=%d per battle, %d per unit; inputs: stance order poll %s, "
             "chase approach registrations %s) | R5 trace: %s (TraceTag=%s, TraceLines=%d per battle, entity tags +0x%X; stance weapon picks, chase "
             "priority / approach / hold, pen hook A verdicts and X) | R8 cost line: %s", cfg_flip ? "on" : "not used (Flip=0)", cfg_flip_changes,
             cfg_flip_win, cfg_flip_lines, LK_UNIT_BLOCKS, poll,
             chase_on ? "on" : "off (chase off)", cfg_trace ? "on" : "no unit traced (Trace=0)", cfg_trace_tag, cfg_trace_lines, g_tag_off, r8);
    return 1;
}

int link_flip_count(void) { return (int)g_flips; }

/* ------------------------------------------------------------------------------------------------ test entry points */
void link_test_force(int suspect_all, int trace_all)
{
    g_force_suspect = suspect_all ? 1 : 0;
    g_force_trace = trace_all ? 1 : 0;
    core_suspect_all = suspect_all ? 1 : 0;
}

void link_test_counts(long *notes, long *traces)
{
    if (notes) *notes = g_notes;
    if (traces) *traces = g_traces;
}

/* the note hook calls per kind (front-order changes, picks, verdicts; declared by the test, not in core.h) */
void link_test_sources(long *front, long *pick, long *verdict)
{
    if (front) *front = g_n_front;
    if (pick) *pick = g_n_pick;
    if (verdict) *verdict = g_n_verd;
}

/* the cost line from given counters through the formatter link_bg_step uses (I-R8; declared by the test, not in core.h) */
int link_test_cost(int64_t hook_qpc, long timed, int64_t orig_qpc, int64_t pen_qpc, long pen_calls, int64_t qpf, int64_t bias_q16, uint32_t now,
                   char *out, int cap)
{
    return cost_text(out, cap, now, hook_qpc, timed, orig_qpc, pen_qpc, pen_calls, qpf, bias_q16);
}

/* 8.4 --replay: every `orders: [N ms] unit P front order now NAME (vt exe+0xV)` line through the real R4 counter with N as the sim
 * clock (no unit is read: the pointers come from an old log), a new battle at every `--- clock ... new mission` line */
int link_test_replay(const char *stance_log, char *units_out, int cap)
{
    if (units_out && cap > 0) units_out[0] = 0;
    FILE *f = stance_log ? fopen(stance_log, "rb") : NULL;
    if (!f) { core_log(CORE_MOD_LINK, "replay: cannot open %s", stance_log ? stance_log : "(null)"); return -1; }
    static char line[16384];
    static unsigned long long seen[512];
    int nseen = 0, blocks = 0;
    lk_reset_battle();
    while (fgets(line, sizeof line, f)) {
        if (strstr(line, "--- clock ") && strstr(line, "new mission")) { lk_reset_battle(); continue; }
        const char *p = strstr(line, "orders: [");
        if (!p) continue;
        unsigned now = 0; unsigned long long up = 0, vt = 0; char nm[LK_NAME + 16];
        if (sscanf(p, "orders: [%u ms] unit %llx front order now %63s", &now, &up, nm) != 3) continue;
        const char *q = strstr(p, "(vt exe+0x");
        if (q) sscanf(q, "(vt exe+0x%llx", &vt);
        if (r4_front((const void *)(uintptr_t)up, 0, (const void *)(uintptr_t)vt, nm, now, 1)) {
            blocks++;
            int i = 0; while (i < nseen && seen[i] != up) i++;
            if (i == nseen && nseen < 512) seen[nseen++] = up;
        }
    }
    fclose(f);
    lk_reset_battle();
    if (units_out && cap > 0)
        for (int i = 0; i < nseen; i++) {
            size_t l = strlen(units_out);
            if (l + 18 >= (size_t)cap) break;
            snprintf(units_out + l, (size_t)cap - l, "%s%016llX", i ? " " : "", seen[i]);
        }
    return blocks;
}
