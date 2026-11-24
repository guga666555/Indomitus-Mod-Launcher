"""Read-only: find vtables (runs of .text pointers in .rdata) that hold at least one function inside an rva range.
usage: python re_vtables.py <exe> <lo hex> <hi hex> [min run, default 6]
Prints each run: rva of the first slot, RTTI class name when the slot before it is a complete-object locator, then slot offset -> rva."""
import struct
import sys


def load(path):
    data = open(path, "rb").read()
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    nsec = struct.unpack_from("<H", data, pe + 6)[0]
    opt = struct.unpack_from("<H", data, pe + 20)[0]
    base = struct.unpack_from("<Q", data, pe + 24 + 24)[0]
    secs, off = [], pe + 24 + opt
    for i in range(nsec):
        name = data[off:off + 8].rstrip(b"\0").decode("latin-1")
        vsize, va, rsize, raw = struct.unpack_from("<IIII", data, off + 8)
        secs.append((name, va, vsize, raw, rsize))
        off += 40
    return data, base, secs


def rva2off(secs, rva):
    for name, va, vsize, raw, rsize in secs:
        if va <= rva < va + rsize:
            return raw + (rva - va)
    return None


def rtti_name(data, base, secs, slot_rva):
    """slot_rva = rva of vtable slot 0; the 8 bytes before it point at the RTTICompleteObjectLocator"""
    o = rva2off(secs, slot_rva - 8)
    if o is None:
        return ""
    loc = struct.unpack_from("<Q", data, o)[0] - base
    lo = rva2off(secs, loc) if 0 < loc < 0x10000000 else None
    if lo is None:
        return ""
    sig, offset, cd, td_rva = struct.unpack_from("<IIII", data, lo)
    to = rva2off(secs, td_rva) if 0 < td_rva < 0x10000000 else None
    if to is None:
        return ""
    name = data[to + 16:to + 16 + 96].split(b"\0")[0].decode("latin-1", "replace")
    return "%s (+%d)" % (name, offset)


def main():
    if len(sys.argv) < 4:
        raise SystemExit(__doc__)
    data, base, secs = load(sys.argv[1])
    lo, hi = int(sys.argv[2], 16), int(sys.argv[3], 16)
    minrun = int(sys.argv[4]) if len(sys.argv) > 4 else 6
    text = [s for s in secs if s[0] == ".text"][0]
    tlo, thi = text[1], text[1] + text[2]
    for name, va, vsize, raw, rsize in secs:
        if name != ".rdata":
            continue
        n = rsize // 8
        vals = struct.unpack_from("<%dQ" % n, data, raw)
        i = 0
        while i < n:
            if tlo <= vals[i] - base < thi:
                j = i
                while j < n and tlo <= vals[j] - base < thi:
                    j += 1
                run = [v - base for v in vals[i:j]]
                if j - i >= minrun and any(lo <= r < hi for r in run):
                    print("vtable at rva %08X  %d slots  %s" % (va + i * 8, j - i, rtti_name(data, base, secs, va + i * 8)))
                    print("   " + "  ".join("+%02X:%X" % (k * 8, r) for k, r in enumerate(run[:24])))
                i = j
            else:
                i += 1


if __name__ == "__main__":
    main()
