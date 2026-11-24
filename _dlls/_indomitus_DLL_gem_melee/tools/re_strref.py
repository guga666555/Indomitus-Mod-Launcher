"""Read-only: find code that references a string in a game exe. usage: python re_strref.py <exe> <ascii text>
Prints each lea / mov with a rip-relative operand that lands on the string, with its containing function (.pdata)."""
import struct
import sys

import re_pdata as R


def main():
    data, base, secs, er, es = R.load(sys.argv[1])
    fl = R.funcs(data, secs, er, es)
    tva, tsize, traw = R.text(secs)
    code = data[traw:traw + tsize]
    needle = sys.argv[2].encode("latin-1")
    hits = []
    for name, va, vsize, raw, rsize in secs:
        if name in (".rdata", ".data"):
            blob = data[raw:raw + rsize]
            i = blob.find(needle)
            while i >= 0:
                hits.append(va + i)
                i = blob.find(needle, i + 1)
    print("string at", ["%X" % h for h in hits])
    for h in hits:
        for i in range(len(code) - 7):
            if code[i] in (0x48, 0x4C) and code[i + 1] == 0x8D and (code[i + 2] & 0xC7) == 0x05:
                if tva + i + 7 + struct.unpack_from("<i", code, i + 3)[0] == h:
                    print("  lea at %X in fn %X" % (tva + i, R.func_of(fl, tva + i)))


if __name__ == "__main__":
    main()
