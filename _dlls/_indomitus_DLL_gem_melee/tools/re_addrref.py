"""Read-only: find rip-relative references to an rva in a game exe and show the instructions around each one.
usage: python re_addrref.py <exe> <rva hex> [context instructions, default 6]"""
import struct
import sys

import capstone

import re_pdata as R


def main():
    data, base, secs, er, es = R.load(sys.argv[1])
    fl = R.funcs(data, secs, er, es)
    tva, tsize, traw = R.text(secs)
    code = data[traw:traw + tsize]
    tgt = int(sys.argv[2], 16)
    ctx = int(sys.argv[3]) if len(sys.argv) > 3 else 6
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    for i in range(len(code) - 7):
        if code[i] in (0x48, 0x4C) and code[i + 1] in (0x8D, 0x8B, 0x89) and (code[i + 2] & 0xC7) == 0x05:
            if tva + i + 7 + struct.unpack_from("<i", code, i + 3)[0] == tgt:
                f = R.func_of(fl, tva + i)
                print("ref at %X in fn %X" % (tva + i, f))
                start = max(f, tva + i - 40) if f else tva + i - 40
                n = 0
                for ins in md.disasm(code[start - tva:start - tva + 120], start):
                    if ins.address >= tva + i - 40:
                        s = ""
                        if "rip" in ins.op_str and ins.mnemonic == "lea":
                            t = ins.address + ins.size + struct.unpack_from("<i", code, ins.address - tva + 3)[0]
                            o = R.rva2off(secs, t)
                            if o:
                                s = "   ; %r" % data[o:o + 28].split(b"\0")[0]
                        print("    %08X  %-6s %s%s" % (ins.address, ins.mnemonic, ins.op_str, s))
                        n += 1
                        if n > ctx * 2:
                            break


if __name__ == "__main__":
    main()
