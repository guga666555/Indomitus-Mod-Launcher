"""Read-only: list the functions (.pdata RUNTIME_FUNCTION entries) of a game exe inside an rva range, with size, direct callees and
(optional) the direct callers of one function.
usage: python re_pdata.py <exe> list <lo hex> <hi hex>          functions in [lo, hi): start, size, direct call targets
       python re_pdata.py <exe> callers <rva hex>               every direct call / jmp to rva: site, containing function
       python re_pdata.py <exe> refs <lo hex> <hi hex> <disp hex> [...]   instructions in [lo, hi) whose memory operand displacement is one of the given values
       python re_pdata.py <exe> vtrefs <rva hex>                .rdata slots (8-byte pointers) that hold image base + rva"""
import struct
import sys

import capstone


def load(path):
    data = open(path, "rb").read()
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    nsec = struct.unpack_from("<H", data, pe + 6)[0]
    opt = struct.unpack_from("<H", data, pe + 20)[0]
    base = struct.unpack_from("<Q", data, pe + 24 + 24)[0]
    exc_rva, exc_size = struct.unpack_from("<II", data, pe + 24 + 112 + 3 * 8)
    secs, off = [], pe + 24 + opt
    for i in range(nsec):
        name = data[off:off + 8].rstrip(b"\0").decode("latin-1")
        vsize, va, rsize, raw = struct.unpack_from("<IIII", data, off + 8)
        secs.append((name, va, vsize, raw, rsize))
        off += 40
    return data, base, secs, exc_rva, exc_size


def rva2off(secs, rva):
    for name, va, vsize, raw, rsize in secs:
        if va <= rva < va + max(vsize, rsize):
            return raw + (rva - va)
    return None


def funcs(data, secs, exc_rva, exc_size):
    o = rva2off(secs, exc_rva)
    out = []
    for i in range(exc_size // 12):
        b, e, u = struct.unpack_from("<III", data, o + i * 12)
        if b:
            out.append((b, e))
    out.sort()
    return out


def text(secs):
    for name, va, vsize, raw, rsize in secs:
        if name == ".text":
            return va, vsize, raw
    raise SystemExit("no .text")


def func_of(fl, rva):
    lo, hi = 0, len(fl) - 1
    while lo <= hi:
        m = (lo + hi) // 2
        if fl[m][0] <= rva < fl[m][1]:
            return fl[m][0]
        if rva < fl[m][0]:
            hi = m - 1
        else:
            lo = m + 1
    return 0


def main():
    if len(sys.argv) < 4:
        raise SystemExit(__doc__)
    data, base, secs, exc_rva, exc_size = load(sys.argv[1])
    fl = funcs(data, secs, exc_rva, exc_size)
    tva, tsize, traw = text(secs)
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    mode = sys.argv[2]
    if mode == "list":
        lo, hi = int(sys.argv[3], 16), int(sys.argv[4], 16)
        for b, e in fl:
            if lo <= b < hi:
                code = data[rva2off(secs, b):rva2off(secs, b) + (e - b)]
                calls = []
                for ins in md.disasm(code, b):
                    if ins.mnemonic == "call" and ins.op_str.startswith("0x"):
                        t = int(ins.op_str, 16)
                        if t not in calls:
                            calls.append(t)
                print("%08X  %5d bytes  calls: %s" % (b, e - b, " ".join("%X" % c for c in calls[:28]) + (" ..." if len(calls) > 28 else "")))
    elif mode == "callers":
        tgt = int(sys.argv[3], 16)
        code = data[traw:traw + tsize]
        i = 0
        while True:
            i = code.find(b"\xE8", i)
            if i < 0:
                break
            if i + 5 <= len(code):
                t = tva + i + 5 + struct.unpack_from("<i", code, i + 1)[0]
                if t == tgt and func_of(fl, tva + i):
                    print("call at %08X in fn %08X" % (tva + i, func_of(fl, tva + i)))
            i += 1
        i = 0
        while True:
            i = code.find(b"\xE9", i)
            if i < 0:
                break
            if i + 5 <= len(code):
                t = tva + i + 5 + struct.unpack_from("<i", code, i + 1)[0]
                if t == tgt and func_of(fl, tva + i):
                    print("jmp  at %08X in fn %08X" % (tva + i, func_of(fl, tva + i)))
            i += 1
    elif mode == "refs":
        lo, hi = int(sys.argv[3], 16), int(sys.argv[4], 16)
        want = {int(x, 16) for x in sys.argv[5:]}
        for b, e in fl:
            if lo <= b < hi:
                code = data[rva2off(secs, b):rva2off(secs, b) + (e - b)]
                md.detail = True
                for ins in md.disasm(code, b):
                    for op in ins.operands:
                        if op.type == capstone.x86.X86_OP_MEM and op.mem.disp in want and op.mem.base not in (capstone.x86.X86_REG_RSP, capstone.x86.X86_REG_RBP, capstone.x86.X86_REG_RIP):
                            print("fn %08X  %08X  %s %s" % (b, ins.address, ins.mnemonic, ins.op_str))
    elif mode == "vtrefs":
        tgt = struct.pack("<Q", base + int(sys.argv[3], 16))
        for name, va, vsize, raw, rsize in secs:
            if name in (".rdata", ".data"):
                blob = data[raw:raw + rsize]
                i = 0
                while True:
                    i = blob.find(tgt, i)
                    if i < 0:
                        break
                    if i % 8 == 0:
                        print("%s slot at rva %08X" % (name, va + i))
                    i += 1


if __name__ == "__main__":
    main()
