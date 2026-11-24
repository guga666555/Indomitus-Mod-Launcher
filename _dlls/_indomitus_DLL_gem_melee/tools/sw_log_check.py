"""Check of the 0.1.5 "new order, not in place" log lines (src\\stance\\stance.c: smooth_why, sw_log, sw_refused, sw_hb).

The stance self-test is silent (its logf_ returns before it formats anything), so a wrong format string in a log line would first
show in the game. This builds the self-test in a temp copy with the log switched on inside the S25 block and checks that the
block's own cases write the lines they should: held off after no scan / after a planning order, the refused merge, Smooth=0, the
stall re-order, another order in front, and the hb: re-aims counters.

Works on a copy (src\\, tools\\ and baseline\\ go to a temp folder and are built there); the real source is never touched.
usage: python tools\\sw_log_check.py [--keep]        exit 1 when the self-test fails or a line is missing
"""
import os
import shutil
import subprocess
import sys
import tempfile

GM = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

ON_BEFORE = "                int o0 = f_orders, mv0 = f_moves, cm0 = f_cms, mg0 = f_merges, rl0 = f_rels, dl0 = f_dels; uint32_t msa = sl->ms, cka = sl->chk_ms;"
OFF_BEFORE = "                *(uint8_t **)(lc.obj + 0xDC8) = NULL; *(void ***)f_mvord = NULL; *ux = 340.0f;"
WANT = [
    ("held off: no scan", "new order, not in place: held off since 0 ms ago because no weapon-pick scan came by"),
    ("held off: planning", "new order, not in place: held off since 0 ms ago because its run order stayed in planning"),
    ("refused merge", "the run order refused the in-place re-aim (flags 0x10004, state 4,"),
    ("Smooth=0", "new order, not in place: the in-place re-aim is off"),
    ("stall re-order", "new order, not in place: a stall re-order is always a new order"),
    ("another order in front", "new order, not in place: another order is in front, not the run order ("),
    ("hb counters", "hb: re-aims ["),
    ("0.1.6 replaced", "^ takes the place of the dll's own run order in front (AddOrder flags 5)"),
    ("0.1.8 appended behind another", "^ appended behind someone else's order in front"),
    ("0.1.8 appended behind its fight", "^ appended behind its own fight order"),
    ("0.1.8 one already waiting", "no order, one of the dll's already waits behind the order in front ("),
    ("0.1.8 one waiting behind fight", "no order, one of the dll's already waits behind its own fight order ("),
]


def main():
    keep = "--keep" in sys.argv
    tmp = tempfile.mkdtemp(prefix="gm_swlog_")
    bad = 0
    try:
        for d in ("src", "tools", "baseline"):
            shutil.copytree(os.path.join(GM, d), os.path.join(tmp, d), ignore=shutil.ignore_patterns("__pycache__"))
        src = os.path.join(tmp, "src", "stance", "stance.c")
        logp = os.path.join(tmp, "sw_log.txt").replace("\\", "/")
        text = open(src, "rb").read().decode("latin-1")
        for anchor in (ON_BEFORE, OFF_BEFORE):
            assert text.count(anchor) == 1, (anchor[:60], text.count(anchor))
        text = text.replace(ON_BEFORE, '                strcpy(g_logpath, "%s"); cfg_log = 1; g_clines = 0;\r\n' % logp + ON_BEFORE)
        text = text.replace(OFF_BEFORE, "                sw_hb(); if (g_log) { fclose(g_log); g_log = NULL; } g_logpath[0] = 0; cfg_log = 0;\r\n" + OFF_BEFORE)
        open(src, "wb").write(text.encode("latin-1"))
        r = subprocess.run([sys.executable, "-B", os.path.join(tmp, "tools", "build.py"), "test-stance"], capture_output=True, cwd=tmp)
        res_p = os.path.join(tmp, "out", "wp1", "stance_test.txt")
        res = open(res_p, "rb").read().decode("latin-1") if os.path.exists(res_p) else ""
        tail = (r.stdout + r.stderr).decode("latin-1", "replace").strip().splitlines()[-1:] or [""]
        if r.returncode != 0 or "FAIL" in res:
            print("the self-test does not pass with the log on (build exit %d: %s)" % (r.returncode, tail[0][:160]))
            for l in res.splitlines():
                if "FAIL" in l:
                    print("   " + l[:160])
            bad += 1
        log = open(logp, encoding="latin-1").read().splitlines() if os.path.exists(logp) else []
        print("%d log lines written by the S25 block" % len(log))
        for name, sub in WANT:
            hits = [l for l in log if sub in l]
            if not hits:
                print("MISSING  %-24s (no line with: %s)" % (name, sub))
                bad += 1
            else:
                print("ok       %-24s %s" % (name, hits[0].strip()[:230]))
        odd = [l for l in log if "(null)" in l or "%" in l]
        for l in odd:
            print("ODD      " + l[:200])
        bad += len(odd)
        print("SW LOG CHECK OK" if not bad else "SW LOG CHECK FAILED: %d" % bad)
    finally:
        if keep:
            print("kept", tmp)
        else:
            shutil.rmtree(tmp, ignore_errors=True)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
