"""Read the [link] flip: blocks of a gem_melee log: per block the order classes, the gaps between changes and how long each
front order stayed. Usage: python flip_stats.py <log>"""
import re
import sys


def main():
    lines = open(sys.argv[1], "rb").read().decode("latin-1").replace("\r\n", "\n").split("\n")
    blocks, cur = [], None
    for l in lines:
        m = re.search(r"\[link\] flip: \[(\d+) ms\] unit (\w+) \((\w+), owner (\d+)", l)
        if m:
            cur = {"t": int(m.group(1)), "unit": m.group(2), "owner": m.group(4), "ch": [], "extra": []}
            blocks.append(cur)
            continue
        if cur is None or "[link]" not in l:
            cur = None if cur is not None and "[link]" not in l else cur
            continue
        m = re.search(r"\[link\]\s+\[(\d+) ms\] (.+?) \(vt", l)
        if m:
            name = m.group(2)
            name = re.sub(r"^\.\?A[UV]e?", "", name).replace("@Order@@", "").replace("@@", "")
            cur["ch"].append((int(m.group(1)), name))
        else:
            cur["extra"].append(l.split("[link]")[1].strip())
    print(len(blocks), "flip blocks")
    stay = {}
    for b in blocks:
        ch = b["ch"]
        seq = " ".join("%s@%d" % (n[:10], t - ch[0][0]) for t, n in ch)
        print("unit %s owner %s: %s" % (b["unit"][-6:], b["owner"], seq))
        for (t0, n0), (t1, _) in zip(ch, ch[1:]):
            stay.setdefault(n0, []).append(t1 - t0)
        for e in b["extra"][:1]:
            print("    ", e[:150])
    print()
    for n, v in sorted(stay.items()):
        v.sort()
        print("front order %-16s stayed: n %3d  min %5d  median %5d  max %5d ms" % (n, len(v), v[0], v[len(v) // 2], v[-1]))


if __name__ == "__main__":
    main()
