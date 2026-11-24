"""Mutation check of the S24 / S25 self-test (intercept lead, in-place re-aim): each mutant breaks one rule of
src\\stance\\stance.c, the stance self-test is rebuilt and must print its "FAIL lead (S24)" line.

Works on a copy: src\\, tools\\ and baseline\\ are copied to a temp folder and built there (out\\ of the copy), so the real
source is never touched and a build running in the work folder is not disturbed (review of the first version, which
rewrote the real file in place).

usage: python tools\\lead_mutants.py [text ...]     every mutant (about 12 minutes), or those with one of the texts in their name
"""
import os
import re
import shutil
import subprocess
import sys
import tempfile

GM = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

MUTANTS = [
    # S24: velocity and lead point
    ("keep limit off", "if (cfg_lead_keep > 0.0f) { float k =", "if (0) { float k ="),
    ("run goal not led", "if (mv && !p->veh && !p->stalled && !lead_held(last_of(p->unit, 0), now)) led = lead_point(", "if (0) led = lead_point("),
    ("order's lead time not kept", "l->lead_s = phase == 1 && led > 0.0f && lspd > 0.0f ? led / lspd : 0.0f;", "l->lead_s = 0.0f;"),
    ("a turn keeps the heading", "< 0.7071f * sqrtf(sp2", "< -2.0f * sqrtf(sp2"),
    ("stall starts no hold", "if (p->stalled) l->nolead_ms = now ? now : 1;", "if (0) l->nolead_ms = now ? now : 1;"),
    ("stall re-order is led", "if (mv && !p->veh && !p->stalled && !lead_held(", "if (mv && !p->veh && !lead_held("),
    ("hold ignored", "&& !p->stalled && !lead_held(last_of(p->unit, 0), now)) led =", "&& !p->stalled) led ="),
    ("stale velocity used", "|| (uint32_t)(now - e->ms) > VEL_STALE_MS || e->vx * e->vx", "|| e->vx * e->vx"),
    ("a sample gap is not noticed", "if (dt > VEL_STALE_MS || sp2 < VEL_MINSPEED", "if (sp2 < VEL_MINSPEED"),
    ("a jump counts as a run", " || sp2 > VEL_MAXSPEED * VEL_MAXSPEED) {", ") {"),
    ("wrong root of the meeting time", "t = t1 > 0.0f && (t2 <= 0.0f || t1 < t2) ? t1 : t2;", "t = t2;"),
    ("LeadSeconds not applied", "if (!(t > 0.0f) || t > cfg_lead_s) t = cfg_lead_s;", "if (!(t > 0.0f)) t = cfg_lead_s;"),
    ("distance cap off", "if (ll > cap) { lx *= cap / ll;", "if (0) { lx *= cap / ll;"),
    ("world origin not checked", "if (ll < 1.0f || (ex + lx) * (ex + lx) + (ey + ly) * (ey + ly) < 1.0f) return 0.0f;", "if (ll < 1.0f) return 0.0f;"),
    ("clock going back not noticed", "if (now < g_vel_clock) { memset(g_vel, 0, sizeof g_vel); g_leadlines = g_smlines = 0; }", "if (0) { memset(g_vel, 0, sizeof g_vel); g_leadlines = g_smlines = 0; }"),
    ("one moving sample is enough", "if (!e || e->n < 2 ||", "if (!e || e->n < 1 ||"),
    ("charge target not sampled", "    if (!veh) vel_note(tgt, now);", "    if (0) vel_note(tgt, now);"),
    ("standing still keeps the velocity", "sp2 < VEL_MINSPEED * VEL_MINSPEED * 0.25f || ", ""),
    ("Lead=0 still leads", "const vel_t *e = cfg_lead ? vel_find(t) : NULL;", "const vel_t *e = vel_find(t);"),
    ("a reused address keeps its entry", "if (k->ent == t && k->id == id) return k;", "if (k->ent == t) return k;"),
    ("the old table hash", "return (uint32_t)(((((uint64_t)(uintptr_t)p >> 4) ^ ((uint64_t)id << 32)) * 0x9E3779B97F4A7C15ull) >> 32);",
     "(void)id; return (uint32_t)(((uintptr_t)p >> 4) * 2654435761u);"),
    # the rule for a new order
    ("new order: no prediction", "if (rem > 0.0f) lead_ahead(tgt, now, rem, gp);", "if (0) lead_ahead(tgt, now, rem, gp);"),
    ("new order: lead time never used up", "float rem = l->lead_s - (float)(uint32_t)(now - l->ms) / 1000.0f;", "float rem = l->lead_s;"),
    ("new order: held past its lead time", "else if (!lead_held(l, now)) lead_point(ent, tgt, now, gp, NULL, NULL);", "else if (0) lead_point(ent, tgt, now, gp, NULL, NULL);"),
    # S25: in-place re-aim
    ("in place while the order plans", "if (st != 3 && st != 4) {", "if (0) {"),
    ("a refused merge is done anyway", "if (ok) ((merge_t)vt[14])(fo, tmp);", "((merge_t)vt[14])(fo, tmp);"),
    ("carrier order not released", "((vfn1_t)tvt[4])(tmp);", ";"),
    ("carrier order not deleted", "((vdel_t)tvt[0])(tmp, 1);", ";"),
    ("no hold after a refusal", "l->nosmooth_ms = now ? now : 1; st_smooth_no++;             /* refused", "st_smooth_no++;             /* refused"),
    ("no hold after a stuck order", "l->nosmooth_ms = now ? now : 1; st_smooth_no++;         /* stuck", "st_smooth_no++;         /* stuck"),
    ("waits for ever on a planning order", "if ((uint32_t)(now - l->wait_ms) < SMOOTH_WAIT_MS) { st_smooth_wait++; return -1; }", "{ st_smooth_wait++; return -1; }"),
    ("someone else's order re-aimed", "fo != (uint8_t *)l->run_order || ", ""),
    ("a stall re-order merged in place", "if (mv && !p->stalled && smooth_can(p->unit, p->tgt, now)) {", "if (mv && smooth_can(p->unit, p->tgt, now)) {"),
    ("in place moves the order's clock", "l->sm_ms = now ? now : 1;      /* the order's own clock stays */", "l->sm_ms = l->ms = now ? now : 1;"),
    ("in-place re-aims not spaced", "if (l->sm_ms && (uint32_t)(now - l->sm_ms) < cfg_smooth_ms) return 0;", ";"),
    ("run order address not kept", "if (ok) { mine_add(order); if (phase == 1) run_o = order; }", "if (ok) { mine_add(order); }"),
    ("in place outside the quant", "if (g_flush_inquant) sm = smooth_reaim(p->unit, p->tgt, xy, now);", "if (1) sm = smooth_reaim(p->unit, p->tgt, xy, now);"),
    ("a kept re-aim never becomes an order", "if ((uint32_t)(now - dl->defer_ms) < SMOOTH_DEFER_MS) { g_sm_defer++; continue; }", "{ g_sm_defer++; continue; }"),
    ("a kept re-aim is not flagged for the scan", "dl->defer_ms) < SMOOTH_DEFER_MS) { g_sm_defer++; continue; }", "dl->defer_ms) < SMOOTH_DEFER_MS) { continue; }"),
    ("a downed unit's order re-aimed", "if (cfg_down_on && vec_has(unit + cfg_enttags, cfg_downtag, 1) == 1) return 0;", ";", 2),
    # Not in the list (tried Nov 21 2026, not caught): the downed-unit or other-enemy test dropped from smooth_can alone. smooth_reaim's own
    # copy masks it inside the quant; from the tick it only holds the new order back by up to SMOOTH_DEFER_MS. A tick flush for a downed
    # unit / another enemy that expects its order at once would pin it.
    ("an order not started re-aimed", " || !(*(const uint32_t *)(fo + 0x18) & 0x10000)) return NULL;", ") return NULL;"),
    ("Smooth=0 ignored", "if (!cfg_smooth || g_smooth < 0 ||", "if (g_smooth < 0 ||"),
    ("the hold does not stop in-place", " || (l->nosmooth_ms && (uint32_t)(now - l->nosmooth_ms) < SMOOTH_HOLD_MS)) return NULL;", ") return NULL;"),
    ("in-place limit = new-order limit", "float thr = d * cfg_smooth_frac > cfg_refresh ? d * cfg_smooth_frac : cfg_refresh,", "float thr = 120.0f,"),
    ("in-place gate not set", "*gate = cfg_smooth_ms;", ";"),
    ("another enemy's run re-aimed in place", " || ref_ent(l->tref) != (const void *)tgt", "", 2),
    ("in-place bookkeeping missing", "if (sm > 0) smooth_done(p, xy, led, now);", ";"),
    ("a vehicle gets the in-place limit", "if (veh) return goal_moved(d, gp[0] - l->ox, gp[1] - l->oy);", ";"),
    # 0.1.6: a charge order on a unit still on the dll's own run order
    ("0.1.6 its own run order not replaced", "if (repl) params[0] = 5;", ";"),
    ("0.1.6 replaced outside the quant", "if (repl && !g_flush_inquant) {", "if (0) {"),
    ("0.1.6 the run order cancels its movement change", "if (mc) { params[0] = 1; own_mc = mc; }", "if (mc) { own_mc = mc; }"),
    ("0.1.6 a front order not of its chain replaced", "if (l->own[i] == (const void *)fo) mine = 1;", "mine = 1;"),
    ("0.1.6 a finished chain still counts", "if (!queue_has(ent, l->last_o)) { l->own_n = 0; l->last_o = NULL; }", "if (0) { }"),
    ("0.1.6 a running fight replaced", "return mine ? (atk ? 2 : 1) : (atk ? 3 : 4);", "return mine ? 1 : (atk ? 3 : 4);"),
    ("0.1.6 ReplaceOwn=0 ignored", "int fk = cfg_replace ? front_kind(p->unit, rl) : 0;", "int fk = front_kind(p->unit, rl);"),
    ("0.1.6 a late order still cancels", "repl = 0; rl->rdefer_ms = 0; st_repl_late++;", "rl->rdefer_ms = 0; st_repl_late++;"),
    ("0.1.8 a second order waits behind another order", "if (fk >= 2 && rl->last_o && front_order(p->unit) != rl->last_o) { sw_skip(p, fk, now); goto pend_done; }", ";"),
    ("0.1.8 its own waiting order counts as the front", "if (fk >= 2 && rl->last_o && front_order(p->unit) != rl->last_o) {", "if (fk >= 2 && rl->last_o) {"),
    ("0.1.8 someone else's order in front is replaced", "repl = fk == 1; late = fk;", "repl = fk >= 1; late = fk;"),
    ("0.1.7 stance off leaves the queue", "    st_stop++;\r\n    return 5;", "    st_stop++;\r\n    return 1;"),
    ("0.1.7 stance off cancels a queue that is not the dll's", "    if (!alive) return 1;", ";"),
    ("0.1.6 the chain is not recorded", "if (any_o) { own_note(l, own_mc); own_note(l, any_o); }", ";"),
    ("0.1.6 a kept order is not flagged for the scan", "< SMOOTH_DEFER_MS) { g_sm_defer++; continue; } }", "< SMOOTH_DEFER_MS) { continue; } }"),
]


def main():
    only = sys.argv[1:]
    tmp = tempfile.mkdtemp(prefix="gm_mutants_")
    bad = ran = 0
    try:
        for d in ("src", "tools", "baseline"):
            shutil.copytree(os.path.join(GM, d), os.path.join(tmp, d), ignore=shutil.ignore_patterns("__pycache__"))
        src = os.path.join(tmp, "src", "stance", "stance.c")
        out = os.path.join(tmp, "out", "wp1", "stance_test.txt")
        text = open(src, "rb").read().decode("latin-1")
        base = subprocess.run([sys.executable, "-B", os.path.join(tmp, "tools", "build.py"), "test-stance"], capture_output=True, cwd=tmp)
        if base.returncode != 0:
            print("the unchanged copy does not pass its self-test:", (base.stdout + base.stderr).decode("latin-1", "replace").strip().splitlines()[-1][:160])
            return 1
        for mut in [m for m in MUTANTS if not only or any(t in m[0] for t in only)]:
            name, old, new = mut[:3]
            want = mut[3] if len(mut) > 3 else 1        # 2: the test stands in smooth_can and in smooth_reaim, the mutant drops both
            ran += 1
            if text.count(old) != want:
                print("%-40s MUTANT NOT APPLIED (%d matches, %d wanted)" % (name, text.count(old), want))
                bad += 1
                continue
            open(src, "wb").write(text.replace(old, new).encode("latin-1"))
            if os.path.exists(out):
                os.remove(out)
            r = subprocess.run([sys.executable, "-B", os.path.join(tmp, "tools", "build.py"), "test-stance"], capture_output=True, cwd=tmp)
            res = open(out, "rb").read().decode("latin-1") if os.path.exists(out) else ""
            m = re.search(r"FAIL lead \(S24\) (0x[0-9A-Fa-f]+)", res)
            if m:
                print("%-40s caught: %s" % (name, m.group(1)))
            else:
                tail = (r.stdout + r.stderr).decode("latin-1", "replace").strip().splitlines()[-1:] or [""]
                print("%-40s NOT CAUGHT (build exit %d: %s)" % (name, r.returncode, tail[0][:120]))
                bad += 1
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print("ALL %d MUTANTS CAUGHT" % ran if not bad else "%d of %d NOT caught" % (bad, ran))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
