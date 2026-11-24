"""Read-only: disassemble a function of a game exe from an rva (capstone), stopping at the first ret past the given length.
usage: python re_fn.py <exe> <rva hex> [max bytes hex, default 180] [--all: do not stop at ret]"""
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
    rva = int(sys.argv[2], 16)
    n = int(sys.argv[3], 16) if len(sys.argv) > 3 and not sys.argv[3].startswith("--") else 0x180
    stop = "--all" not in sys.argv
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    off = rva2off(secs, rva)
    for ins in md.disasm(data[off:off + n], rva):
        print("    %08X  %-8s %s" % (ins.address, ins.mnemonic, ins.op_str))
        if stop and ins.mnemonic in ("ret", "int3"):
            break


if __name__ == "__main__":
    main()
