"""Read-only: which flag words the engine's own callers pass to eActor::AddOrder(actor, order, params{flags, stamp}).

For every `call AddOrder` site of an exe the 0x60 bytes before it are searched for `lea r8, [rsp+d]` / `lea r8, [rbp+d]` and a
`mov dword ptr [rsp+d], imm32` / `mov dword ptr [rbp+d], imm32` with the same displacement: imm32 = params[0]. Prints a count per
value and the call sites of each (rva). A site whose flags come from a register (computed) is listed as "computed".
usage: python re_addorder_flags.py <exe> <AddOrder rva hex>"""
import collections
import struct
import sys


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


def main():
    data, secs = load(sys.argv[1])
    target = int(sys.argv[2], 16)
    text = [s for s in secs if s[0] == ".text"][0]
    va, raw, size = text[1], text[3], text[4]
    found = collections.defaultdict(list)
    i = data.find(b"\xE8", raw)
    end = raw + size
    while i != -1 and i < end - 5:
        rva = va + (i - raw)
        if (rva + 5 + struct.unpack_from("<i", data, i + 1)[0]) & 0xFFFFFFFF == target:
            win = data[i - 0x60:i]
            disp = None
            for k in range(len(win) - 5, -1, -1):
                if win[k:k + 4] == b"\x4C\x8D\x44\x24":            # lea r8, [rsp+d8]
                    disp = ("rsp", win[k + 4]); break
                if win[k:k + 3] == b"\x4C\x8D\x45":                # lea r8, [rbp+d8]
                    disp = ("rbp", win[k + 3]); break
                if win[k:k + 4] == b"\x4C\x8D\x84\x24":            # lea r8, [rsp+d32]
                    disp = ("rsp32", struct.unpack_from("<I", win, k + 4)[0]); break
            val = "computed / not found"
            if disp:
                for k in range(len(win) - 8, -1, -1):
                    if disp[0] == "rsp" and win[k:k + 3] == b"\xC7\x44\x24" and win[k + 3] == disp[1]:
                        val = "0x%X" % struct.unpack_from("<I", win, k + 4)[0]; break
                    if disp[0] == "rbp" and win[k:k + 2] == b"\xC7\x45" and win[k + 2] == disp[1]:
                        val = "0x%X" % struct.unpack_from("<I", win, k + 3)[0]; break
                    if disp[0] == "rsp32" and win[k:k + 3] == b"\xC7\x84\x24" and struct.unpack_from("<I", win, k + 3)[0] == disp[1]:
                        val = "0x%X" % struct.unpack_from("<I", win, k + 7)[0]; break
            found[val].append(rva)
        i = data.find(b"\xE8", i + 1)
    total = sum(len(v) for v in found.values())
    print("%d call sites of exe+0x%X" % (total, target))
    for val, sites in sorted(found.items(), key=lambda kv: -len(kv[1])):
        print("  params[0] = %-22s %3d sites: %s%s" % (val, len(sites), " ".join("%X" % s for s in sites[:14]), " ..." if len(sites) > 14 else ""))


if __name__ == "__main__":
    main()
