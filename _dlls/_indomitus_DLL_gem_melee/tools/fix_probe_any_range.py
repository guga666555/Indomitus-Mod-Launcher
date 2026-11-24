"""Nov 21 2026 (Alex: "why is this back at 1"): re-apply his Nov 15 change (commit c4dd98a, ProbeAnyRange=0 with its two
comment lines) to MeleeStance's SOURCE ini in the mc054 worktree and to the dist copy next to it.

Guga's main took that commit's payload ini (launcher/payload/gem_meleestance.ini = 0) but not its source ini, so the 0.7.42
builds made from this worktree copied ProbeAnyRange=1 into tonight's test launchers (build.bat copies src ini -> dist).

Dry run by default; --apply writes. CRLF kept. Backups: Documents/Indomitus_backups/_tools/gem_melee/bak_probe_any_range/.
"""
import os
import shutil
import sys

ROOT = "C:/Users/alex_/Documents/IndomitusLauncher_mc054/_dlls/_indomitus_DLL_meleestance"
FILES = [ROOT + "/src/gem_meleestance.ini", ROOT + "/dist/gem_meleestance.ini"]
BAK = "C:/Users/alex_/Documents/Indomitus_backups/_tools/gem_melee/bak_probe_any_range"
OLD = b"\r\nProbeAnyRange=1\r\n"
NEW = (b"\r\n; 0.7.41 ini (Nov 15 2026, Alex): 0 = probe only inside Distance. At 1 the first-sighting probe refused every gun for one pick at any range,"
       b"\r\n; bazookas and AT grenades included, and a unit under an attack order often never picked again (AI ignored tanks in range)."
       b"\r\nProbeAnyRange=0\r\n")


def main():
    apply = "--apply" in sys.argv
    for p in FILES:
        b = open(p, "rb").read()
        if b"\r\nProbeAnyRange=0\r\n" in b:
            print("already 0:", p)
            continue
        if b.count(OLD) != 1:
            print("SKIP (ProbeAnyRange=1 line not found exactly once):", p)
            continue
        out = b.replace(OLD, NEW)
        assert out.count(b"\n") == out.count(b"\r\n") == b.count(b"\r\n") + 2
        print(("edit " if apply else "would ") + p, "line", b[:b.index(OLD) + 2].count(b"\n") + 1)
        if apply:
            os.makedirs(BAK, exist_ok=True)
            dst = BAK + "/" + os.path.basename(os.path.dirname(p)) + "_" + os.path.basename(p)
            if not os.path.exists(dst):
                shutil.copy2(p, dst)
            open(p, "wb").write(out)
    if not apply:
        print("dry run: nothing written (--apply)")


if __name__ == "__main__":
    main()
