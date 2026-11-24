"""Read-only: list the vtables of every class whose RTTI name contains a text, with the address in one slot.
usage: python re_rtti.py <exe> <text in the mangled name> [slot, default 18]"""
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


def off2rva(secs, o):
    for name, va, vsize, raw, rsize in secs:
        if raw <= o < raw + rsize:
            return va + (o - raw)
    return None


def rva2off(secs, rva):
    for name, va, vsize, raw, rsize in secs:
        if va <= rva < va + max(vsize, rsize):
            return raw + (rva - va)
    return None


def main():
    data, base, secs = load(sys.argv[1])
    want = sys.argv[2].encode("latin-1")
    slot = int(sys.argv[3]) if len(sys.argv) > 3 else 18
    rdata = [s for s in secs if s[0] == ".rdata"][0]
    r0, r1 = rdata[3], rdata[3] + rdata[4]
    # type descriptors: name at +0x10, starts with ".?A"
    tds = {}
    i = data.find(b".?A")
    while i != -1:
        end = data.find(b"\0", i)
        name = data[i:end]
        if want in name and len(name) < 200:
            rva = off2rva(secs, i - 0x10)
            if rva is not None:
                tds[rva] = name.decode("latin-1")
        i = data.find(b".?A", i + 3)
    # complete object locators: signature 1, +0xC = type descriptor rva, +0x14 = self rva
    for o in range(r0, r1 - 0x18, 4):
        if struct.unpack_from("<I", data, o)[0] != 1:
            continue
        td = struct.unpack_from("<I", data, o + 0xC)[0]
        if td not in tds:
            continue
        self_rva = struct.unpack_from("<I", data, o + 0x14)[0]
        if off2rva(secs, o) != self_rva:
            continue
        offset = struct.unpack_from("<I", data, o + 4)[0]
        ptr = struct.pack("<Q", base + self_rva)
        j = data.find(ptr, r0, r1)
        while j != -1:
            vt_rva = off2rva(secs, j + 8)
            fn = struct.unpack_from("<Q", data, j + 8 + 8 * slot)[0] - base
            print("%-44s vtable exe+0x%X (this offset 0x%X)  slot %d -> exe+0x%X" % (tds[td], vt_rva, offset, slot, fn))
            j = data.find(ptr, j + 8, r1)


if __name__ == "__main__":
    main()
