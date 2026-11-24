"""Read-only: the function that contains an rva (from .pdata) and every direct call to it, with the instructions before each call.
usage: python re_callers.py <exe> <rva hex inside the function> [instructions before each call, default 14]"""
import struct
import sys

import capstone


def load(path):
    data = open(path, "rb").read()
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    nsec = struct.unpack_from("<H", data, pe + 6)[0]
    opt = struct.unpack_from("<H", data, pe + 20)[0]
    secs, off = [], pe + 24 + opt
    for i in range(nsec):
        name = data[off:off + 8].rstrip(b"\0").decode()
        vsize, va, rsize, raw = struct.unpack_from("<IIII", data, off + 8)
        secs.append((name, va, vsize, raw, rsize))
        off += 40
    return data, secs


def main():
    path, rva = sys.argv[1], int(sys.argv[2], 16)
    nb = int(sys.argv[3]) if len(sys.argv) > 3 else 14
    data, secs = load(path)

    def off(r):
        for _, va, vs, raw, rs in secs:
            if va <= r < va + max(vs, rs):
                return raw + (r - va)
        raise SystemExit("rva %X not mapped" % r)
    pd = next(s for s in secs if s[0] == ".pdata")
    start = end = None
    for i in range(pd[3], pd[3] + pd[2] - 11, 12):
        b, e, _ = struct.unpack_from("<III", data, i)
        if b <= rva < e:
            start, end = b, e
            break
    print("function %X..%X" % (start, end))
    tx = next(s for s in secs if s[0] == ".text")
    t0, t1 = tx[3], tx[3] + tx[4]
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    calls = []
    i = t0
    blob = data[t0:t1]
    j = blob.find(b"\xE8")
    while j >= 0:
        site = tx[1] + j
        tgt = site + 5 + struct.unpack_from("<i", blob, j + 1)[0]
        if tgt == start:
            calls.append(site)
        j = blob.find(b"\xE8", j + 1)
    print(len(calls), "direct calls")
    for site in calls:
        # walk back to a point that decodes cleanly onto the call
        best = None
        for back in range(nb * 7, 8, -1):
            ins = list(md.disasm(data[off(site - back):off(site) + 5], site - back))
            if ins and ins[-1].address == site and ins[-1].mnemonic == "call":
                best = ins
                break
        print("---- call at %X" % site)
        for x in (best or [])[-nb:]:
            print("    %08X  %-8s %s" % (x.address, x.mnemonic, x.op_str))


if __name__ == "__main__":
    main()
