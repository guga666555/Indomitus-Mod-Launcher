"""Read-only: find the code that asserts quantControl.InsideQuant() in the editor exe and show what it tests.
usage: python re_quant.py <exe>"""
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
        name = data[off:off + 8].rstrip(b"\0").decode("latin-1")
        vsize, va, rsize, raw = struct.unpack_from("<IIII", data, off + 8)
        secs.append((name, va, vsize, raw, rsize))
        off += 40
    return data, secs


def off2rva(secs, o):
    for name, va, vsize, raw, rsize in secs:
        if raw <= o < raw + rsize:
            return va + (o - raw)


def rva2off(secs, rva):
    for name, va, vsize, raw, rsize in secs:
        if va <= rva < va + max(vsize, rsize):
            return raw + (rva - va)


def main():
    data, secs = load(sys.argv[1])
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    key = b"quantControl.InsideQuant()\0"
    hits, i = [], data.find(key)
    while i >= 0:
        hits.append(off2rva(secs, i)); i = data.find(key, i + 1)
    print("string at", ["exe+0x%X" % h for h in hits])
    text = [s for s in secs if s[0] == ".text"][0]
    _, va, vsize, raw, rsize = text
    blob = data[raw:raw + rsize]
    refs = []
    for h in hits:
        # lea reg, [rip+disp32]: 48/4C 8D /r with modrm rip-relative
        j = 0
        while True:
            j = blob.find(b"\x8D", j)
            if j < 0:
                break
            if j >= 1 and blob[j - 1] in (0x48, 0x4C) and (blob[j + 1] & 0xC7) == 0x05:
                disp = struct.unpack_from("<i", blob, j + 2)[0]
                if va + j + 6 + disp == h:
                    refs.append(va + j - 1)
            j += 1
    print(len(refs), "references:", ["exe+0x%X" % r for r in refs[:40]])
    for r in refs[:40]:
        # the assert line number is loaded near the reference: mov edx, imm32
        off = rva2off(secs, r - 0x40)
        ins = list(md.disasm(data[off:off + 0x70], r - 0x40))
        line = [x for x in ins if x.mnemonic == "mov" and x.op_str.startswith("edx, 0x")]
        if any(x.op_str == "edx, 0x182" for x in line):
            print("== ePP_Cost.cpp:386 assert at exe+0x%X; code before it:" % r)
            off = rva2off(secs, r - 0x60)
            for x in md.disasm(data[off:off + 0x80], r - 0x60):
                print("    %08X  %-8s %s" % (x.address, x.mnemonic, x.op_str))
            break


if __name__ == "__main__":
    main()
