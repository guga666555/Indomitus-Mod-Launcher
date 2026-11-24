"""Read-only look at the editor exe: who calls the move-order factory (MakeMove) and how the order is then sent.
usage: python re_follow.py <exe> <MakeMove rva hex> [AddOrder rva hex] [--at <rva hex> <bytes>]"""
import struct
import sys

import capstone


def load(path):
    data = open(path, "rb").read()
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    nsec = struct.unpack_from("<H", data, pe + 6)[0]
    opt = struct.unpack_from("<H", data, pe + 20)[0]
    base = struct.unpack_from("<Q", data, pe + 24 + 24)[0]
    secs = []
    off = pe + 24 + opt
    for i in range(nsec):
        name = data[off:off + 8].rstrip(b"\0").decode("latin-1")
        vsize, va, rsize, raw = struct.unpack_from("<IIII", data, off + 8)
        secs.append((name, va, vsize, raw, rsize))
        off += 40
    return data, base, secs


def rva2off(secs, rva):
    for name, va, vsize, raw, rsize in secs:
        if va <= rva < va + max(vsize, rsize):
            return raw + (rva - va)
    return None


def disasm(md, data, secs, rva, n, mark=None):
    off = rva2off(secs, rva)
    out = []
    for ins in md.disasm(data[off:off + n], rva):
        out.append("%s  %08X  %-8s %s" % (">>" if mark is not None and ins.address == mark else "  ", ins.address, ins.mnemonic, ins.op_str))
    return out


def func_start(data, secs, rva):
    """walk back to the previous int3 padding (CC CC) or ret+padding: a cheap function start guess"""
    off = rva2off(secs, rva)
    for back in range(0, 0x3000):
        o = off - back
        if data[o - 1] == 0xCC and data[o] != 0xCC:
            return rva - back
    return rva


def main():
    path = sys.argv[1]
    data, base, secs = load(path)
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    if "--at" in sys.argv:
        i = sys.argv.index("--at")
        rva, n = int(sys.argv[i + 1], 16), int(sys.argv[i + 2], 16)
        print("\n".join(disasm(md, data, secs, rva, n)))
        return
    mk = int(sys.argv[2], 16)
    add = int(sys.argv[3], 16) if len(sys.argv) > 3 else None
    text = [s for s in secs if s[0] == ".text"][0]
    _, va, vsize, raw, rsize = text
    blob = data[raw:raw + rsize]
    sites = []
    i = 0
    while True:
        i = blob.find(b"\xE8", i)
        if i < 0:
            break
        if i + 5 <= len(blob):
            rel = struct.unpack_from("<i", blob, i + 1)[0]
            if va + i + 5 + rel == mk:
                sites.append(va + i)
        i += 1
    print("image base %X, .text rva %X size %X; %d calls of exe+0x%X" % (base, va, vsize, len(sites), mk))
    for s in sites:
        fs = func_start(data, secs, s)
        # the calls made in the 0x60 bytes after this site
        after = []
        off = rva2off(secs, s + 5)
        for ins in md.disasm(data[off:off + 0x70], s + 5):
            if ins.mnemonic == "call":
                after.append(ins.op_str)
            if ins.mnemonic in ("ret", "int3"):
                break
        tag = ""
        if add is not None and ("0x%x" % add) in after:
            tag = "  <- then AddOrder"
        print("call at exe+0x%X (function ~exe+0x%X), calls after it: %s%s" % (s, fs, ", ".join(after[:5]), tag))


if __name__ == "__main__":
    main()
