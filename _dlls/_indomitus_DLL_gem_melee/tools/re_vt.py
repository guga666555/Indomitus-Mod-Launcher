"""Read-only: print vtable slots of a class in an exe and disassemble some of them.
usage: python re_vt.py <exe> <vtable rva hex> <slot,slot,...> [bytes per function, hex]"""
import struct
import sys

import capstone


def load(path):
    data = open(path, "rb").read()
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    nsec = struct.unpack_from("<H", data, pe + 6)[0]
    opt = struct.unpack_from("<H", data, pe + 20)[0]
    base = struct.unpack_from("<Q", data, pe + 24 + 24)[0]
    secs, off = [], pe + 24 + opt
    for i in range(nsec):
        vsize, va, rsize, raw = struct.unpack_from("<IIII", data, off + 8)
        secs.append((va, vsize, raw, rsize))
        off += 40
    return data, base, secs


def rva2off(secs, rva):
    for va, vsize, raw, rsize in secs:
        if va <= rva < va + max(vsize, rsize):
            return raw + (rva - va)
    raise SystemExit("rva %X not mapped" % rva)


def main():
    data, base, secs = load(sys.argv[1])
    vt = int(sys.argv[2], 16)
    slots = [int(x) for x in sys.argv[3].split(",")]
    n = int(sys.argv[4], 16) if len(sys.argv) > 4 else 0x90
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    for s in slots:
        ptr = struct.unpack_from("<Q", data, rva2off(secs, vt + 8 * s))[0]
        rva = ptr - base
        print("== slot %d (+0x%X) -> exe+0x%X" % (s, 8 * s, rva))
        off = rva2off(secs, rva)
        for ins in md.disasm(data[off:off + n], rva):
            print("    %08X  %-8s %s" % (ins.address, ins.mnemonic, ins.op_str))
            if ins.mnemonic in ("ret", "int3") or (ins.mnemonic == "jmp" and not ins.op_str.startswith("0x")):
                break


if __name__ == "__main__":
    main()
