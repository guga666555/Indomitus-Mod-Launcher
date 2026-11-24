"""gem_melee port diff guard (spec 4.5).

usage: python tools\\port_check.py all|stance|chase|pen
       python tools\\port_check.py --rehash all|stance|chase|pen     (after a REVIEWED edit: rewrite the rules' sha256 fields)

For every file of tools\\port_rules\\<mod>.json the port is diffed against its snapshot (difflib.SequenceMatcher, autojunk off,
on LF-normalised lines). Every non-equal hunk must be allowed by a rule:
  - every deleted / replaced snapshot line i1+1..i2 lies inside ONE rule's range [from, to] (1-based, inclusive);
  - a pure insertion before snapshot line i1+1 needs a rule with from <= i1+1 <= to+1 (to = from-1: an insertion-only rule);
  - the lines a hunk adds are charged to that rule.
Each rule must be used exactly as recorded (review finding F3: an upper bound alone let a removed or reverted edit, and any
content change inside a rule's lines, pass):
  - at least one hunk is charged to it, and the lines it adds == add_max;
  - "sha256" = SHA-256 of what the rule's hunks do (for each hunk: the deleted snapshot range and the added lines, LF-joined);
    a different edit inside the rule fails until the rule is reviewed and --rehash writes the new value (never automatic).
The port's line endings must match "eol" (crlf / lf). Output per file: "<port> OK (n hunks[, rules used: ...])" or the offending
hunks; the full unified diff of the module goes to out\\port\\<mod>.diff. Exit 1 on any violation (--rehash: only on a hunk no rule
allows, or a line-count mismatch).
"""
import difflib
import hashlib
import json
import sys
from pathlib import Path

GM = Path(__file__).resolve().parents[1]
RULES = GM / "tools" / "port_rules"
MODS = ["stance", "chase", "pen"]


def read_lines(p: Path):
    raw = p.read_bytes()
    text = raw.decode("latin-1")
    return raw, text.replace("\r\n", "\n").split("\n")


def eol_ok(raw: bytes, eol: str):
    lone_lf = raw.replace(b"\r\n", b"").count(b"\n")
    crlf = raw.count(b"\r\n")
    if eol == "crlf":
        return lone_lf == 0, f"{lone_lf} LF-only line ending(s)"
    if eol == "lf":
        return crlf == 0, f"{crlf} CRLF line ending(s)"
    return False, f"unknown eol '{eol}'"


def check_file(entry, diff_out, rehash):
    snap_p, port_p = GM / entry["snapshot"], GM / entry["port"]
    name = entry["port"]
    if not snap_p.exists():
        return False, [f"{name}: snapshot {entry['snapshot']} missing"], 0
    if not port_p.exists():
        return False, [f"{name}: port missing"], 0
    _, snap = read_lines(snap_p)
    raw, port = read_lines(port_p)
    msgs = []
    ok = True
    good, why = eol_ok(raw, entry.get("eol", "crlf"))
    if not good:
        ok = False
        msgs.append(f"  line endings: want {entry.get('eol')}, found {why}")
    rules = entry.get("rules", [])
    used = {r["id"]: 0 for r in rules}
    nh = {r["id"]: 0 for r in rules}
    content = {r["id"]: [] for r in rules}
    order = []
    sm = difflib.SequenceMatcher(None, snap, port, autojunk=False)
    hunks = 0
    for tag, i1, i2, j1, j2 in sm.get_opcodes():
        if tag == "equal":
            continue
        hunks += 1
        added = j2 - j1
        rule = None
        if i2 > i1:                                            # deleted / replaced lines i1+1..i2
            for r in rules:
                if r["from"] <= i1 + 1 and i2 <= r["to"]:
                    rule = r
                    break
            where = f"snapshot lines {i1 + 1}-{i2} {tag}d" if tag == "delete" else f"snapshot lines {i1 + 1}-{i2} replaced by port lines {j1 + 1}-{j2}"
        else:                                                  # pure insertion before snapshot line i1+1
            for r in rules:
                if r["from"] <= i1 + 1 <= r["to"] + 1:
                    rule = r
                    break
            where = f"{added} line(s) inserted before snapshot line {i1 + 1} (port lines {j1 + 1}-{j2})"
        if rule is None:
            ok = False
            msgs.append(f"  hunk {hunks}: {where}: no rule allows it")
            for l in snap[i1:i2][:6]:
                msgs.append(f"    - {l[:150]}")
            for l in port[j1:j2][:6]:
                msgs.append(f"    + {l[:150]}")
            continue
        used[rule["id"]] += added
        nh[rule["id"]] += 1
        content[rule["id"]].append(f"-{i1 + 1}..{i2}")
        content[rule["id"]].extend("+" + l for l in port[j1:j2])
        if rule["id"] not in order:
            order.append(rule["id"])
    changed = 0
    for r in rules:
        rid = r["id"]
        if not nh[rid]:
            ok = False
            msgs.append(f"  rule {rid} (snapshot {r['from']}-{r['to']}): no hunk uses it (the edit was removed or reverted to the snapshot text)")
        if used[rid] != r["add_max"]:
            ok = False
            msgs.append(f"  rule {rid} (snapshot {r['from']}-{r['to']}): {used[rid]} lines added, the rule records {r['add_max']}")
        sha = hashlib.sha256("\n".join(content[rid]).encode("latin-1")).hexdigest()
        if rehash:
            if r.get("sha256") != sha:
                r["sha256"] = sha
                changed += 1
                msgs.append(f"  rule {rid}: sha256 {'set' if 'sha256' not in r else 'rewritten'}")
        elif r.get("sha256") != sha:
            ok = False
            msgs.append(f"  rule {rid} (snapshot {r['from']}-{r['to']}): content differs from the reviewed edit (sha256 {sha[:16]}.., rules file "
                        f"{str(r.get('sha256'))[:16]}..): review it, then `port_check.py --rehash`")
    diff = difflib.unified_diff(snap, port, fromfile=entry["snapshot"], tofile=entry["port"], lineterm="", n=3)
    diff_out.extend(diff)
    if ok:
        head = f"{name} OK ({hunks} hunks" + (f", rules used: {' '.join(order)})" if order else ")")
    else:
        head = f"{name} FAILED ({hunks} hunks)"
    return ok, [head] + msgs, changed


def check_mod(mod, rehash=False):
    f = RULES / f"{mod}.json"
    if not f.exists():
        print(f"{mod}: {f.relative_to(GM)} missing")
        return False
    data = json.loads(f.read_text(encoding="utf-8"))
    ids = [r["id"] for e in data["files"] for r in e.get("rules", [])]
    dup = sorted({i for i in ids if ids.count(i) > 1})
    all_ok = True
    if dup:
        print(f"{mod}: duplicate rule ids {dup}")
        all_ok = False
    print(f"{mod}:")
    diff_lines = []
    changed = 0
    for entry in data["files"]:
        ok, msgs, ch = check_file(entry, diff_lines, rehash)
        changed += ch
        if rehash:                                             # --rehash fails only on what a hash cannot fix
            ok = not any("no rule allows it" in m or "lines added, the rule records" in m or "no hunk uses it" in m or "line endings" in m
                         for m in msgs)
        all_ok &= ok
        for m in msgs:
            print(("  " if not m.startswith("  ") else "") + m)
    if rehash and changed and all_ok:
        f.write_text(json.dumps(data, indent=1) + "\n", encoding="utf-8", newline="\n")
        print(f"  {f.relative_to(GM)}: {changed} sha256 field(s) written")
    out = GM / "out" / "port"
    out.mkdir(parents=True, exist_ok=True)
    (out / f"{mod}.diff").write_text("\n".join(diff_lines) + ("\n" if diff_lines else ""), encoding="latin-1", newline="\n")
    return all_ok


def main() -> int:
    args = sys.argv[1:]
    rehash = bool(args) and args[0] == "--rehash"
    if rehash:
        args = args[1:]
    arg = args[0] if args else ""
    if arg not in MODS + ["all"] or len(args) != 1:
        print(__doc__)
        return 2
    ok = True
    for m in (MODS if arg == "all" else [arg]):
        ok &= check_mod(m, rehash)
    if rehash:
        print("PORT REHASH OK" if ok else "PORT REHASH FAILED")
    else:
        print("PORT CHECK OK" if ok else "PORT CHECK FAILED")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
