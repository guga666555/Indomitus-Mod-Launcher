"""Main-session check (T3 / T4), independent of tools/parity.py: every engine address the three old dlls
printed when they installed in a real process must show up in gem_melee's log for the same exe.

It reads only log text. From each old log it takes the install block of the LAST session (the lines up to
the first run-time line) and collects every `exe+0x...` address; from the new text it takes every line (or
a line range). An old address the new text lacks is reported with the old line it came from.

usage:
  python live_addr_check.py --new <file> [--lines A-B] --stance <old log> --chase <old log> --pen <old log>
  python live_addr_check.py --preset editor|game|server --new <file> [--lines A-B]
        editor = inputs/live_payload_a1d6ed3c (MeleeStance 0.7.41, MeleeChase 0.5.4, PenFilter 0.1.1, Nov 20 12:16)
        game / server = inputs/live_payload_5696d011 (MeleeStance 0.7.42, 0.5.4, 0.1.1, hosted game Nov 20 21:25)
  python live_addr_check.py --preset <p> --self          old logs against themselves (tool sanity)
"""
import argparse
import io
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
INP = os.path.join(os.path.dirname(HERE), "inputs")
PRESETS = {
    "editor": {"stance": "live_payload_a1d6ed3c/gem_meleestance_editor.log",
               "chase": "live_payload_a1d6ed3c/gem_meleechase.log",
               "pen": "live_payload_a1d6ed3c/gem_penfilter_editor.log"},
    "game": {"stance": "live_payload_5696d011/game/gem_meleestance.log",
             "chase": "live_payload_5696d011/game/gem_meleechase.log",
             "pen": "live_payload_5696d011/game/gem_penfilter.log"},
    "server": {"stance": "live_payload_5696d011/server/gem_meleestance_server.log",
               "chase": "live_payload_5696d011/server/gem_meleechase.log",
               "pen": "live_payload_5696d011/server/gem_penfilter_server.log"},
}
# first run-time line of each module's log = end of its install block
STOP = {
    "stance": re.compile(r"^(raw:|hb:|scan:|charge:|stance:|order:|orders:|poll:|tick:|no-melee:|zerg:|---)"),
    "chase": re.compile(r"simulation change: every player"),
    "pen": re.compile(r"^(A|P|B|X) \[\d+ ms\]|^stats \[|^new battle"),
}
ADDR = re.compile(r"exe\+0x([0-9A-Fa-f]+)")


def read(path):
    return io.open(path, encoding="utf-8", errors="replace").read().splitlines()


def strip_prefix(ln):
    ln = re.sub(r"^\d\d:\d\d:\d\d\.\d+\s+", "", ln)                     # wall time stamp (chase, gem_melee)
    ln = re.sub(r"^\d+\s+(?=\[(core|chase|stance|pen|link)\])", "", ln)  # gem_melee sim ms column
    ln = re.sub(r"^\[GEM MeleeChase\] ", "", ln)
    ln = re.sub(r"^\[(core|chase|stance|pen|link)\]\s?", "", ln)
    return ln


def install_block(mod, lines):
    lines = [strip_prefix(x) for x in lines]
    if mod == "pen":
        idx = [i for i, ln in enumerate(lines) if "finders:" in ln]
        blk = lines[idx[-1]:] if idx else lines
    elif mod == "chase":
        idx = [i for i, ln in enumerate(lines) if "loaded into" in ln]
        blk = lines[idx[-1]:] if idx else lines
    else:
        idx = [i for i, ln in enumerate(lines) if "] installed: ammo match" in ln]
        blk = lines
        if idx:
            j = idx[-1]
            while j > 0 and not STOP[mod].search(lines[j - 1]):
                j -= 1                                             # stance prints finder lines before "installed:"
            blk = lines[j:]
    out = []
    for ln in blk:
        hit = STOP[mod].search(ln)
        if hit and mod != "chase":
            # stance's own "charge:" finder lines open its block: only stop on run-time lines after "installed:"
            if mod == "stance" and not any("] installed: ammo match" in x for x in out):
                out.append(ln)
                continue
            break
        out.append(ln)
        if hit:
            break
    return out


def addrs(lines):
    s = set()
    for ln in lines:
        for m in ADDR.finditer(ln):
            s.add(m.group(1).upper().lstrip("0") or "0")
    return s


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--preset", choices=sorted(PRESETS))
    ap.add_argument("--stance"), ap.add_argument("--chase"), ap.add_argument("--pen")
    ap.add_argument("--new"), ap.add_argument("--lines"), ap.add_argument("--self", action="store_true")
    a = ap.parse_args()
    old_paths = {m: os.path.join(INP, p) for m, p in PRESETS[a.preset].items()} if a.preset else {}
    for m in ("stance", "chase", "pen"):
        if getattr(a, m):
            old_paths[m] = getattr(a, m)
    if len(old_paths) != 3:
        ap.error("give --preset or all of --stance --chase --pen")
    old = {m: install_block(m, read(p)) for m, p in old_paths.items()}
    if a.self:
        new_lines = [x for m in old for x in old[m]]
    else:
        new_lines = read(a.new)
        if a.lines:
            lo, hi = (int(x) for x in a.lines.split("-"))
            new_lines = new_lines[lo - 1:hi]
        new_lines = [strip_prefix(x) for x in new_lines]
    new = addrs(new_lines)
    bad = 0
    for mod, lines in old.items():
        want = addrs(lines)
        miss = sorted(want - new, key=lambda h: int(h, 16))
        print(f"{mod}: {len(lines)} install line(s), {len(want)} address(es) in the old log, {len(miss)} missing in the new text")
        for h in miss:
            src = next(ln for ln in lines if re.search(r"exe\+0x0*" + h + r"\b", ln, re.I))
            print(f"  MISSING exe+0x{h}   old line: {src[:170]}")
        bad += len(miss)
    print("LIVE ADDRESSES OK" if not bad else f"LIVE ADDRESSES: {bad} MISSING")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
