"""Read-only check of the three game exes for the in-place re-aim of a running move order (gem_melee S25).

The engine's squad-follow code does not give a follower a new order: it builds an Order::eMoveTask, asks the unit's front order
`bool vt[13](front, new)` (front order active = flags +0x18 bit 0x10000, and the same class) and then calls
`void vt[14](front, new)`, which, while the front order is moving (state +0x3C = 3 or 4), takes over the new order's destination,
plans the path and merges it into the running path-follow sub-order; the new order is then released (vt[4]) and deleted (vt[0]).
The stance charge re-aims its run the same way. This script shows that the three exes agree on what the dll relies on:
  - the eMoveTask vtable is found through RTTI
  - slot 13 starts with   test dword ptr [rcx+18h], 10000h
  - slot 14 calls slot 13 on itself, casts the other order and tests   [this+3Ch] == 3 or 4
  - slot 4 tests bit 16 of [rcx+18h] first (a no-op for an order that was never started)
usage: python tools\\smooth_check.py [folder with the exes]     prints one block per exe, exit 1 on any mismatch
"""
import os
import struct
import sys

DEFAULT_BIN = r"C:\Program Files (x86)\Steam\steamapps\common\Call to Arms - Gates of Hell\binaries\x64"
EXES = ("call_to_arms.exe", "call_to_arms_ed.exe", "call_to_arms_server.exe")
NAME = b".?AUeMoveTask@Order@@\0"


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


def find_vtable(data, base, secs):
    i = data.find(NAME)
    if i < 0 or data.find(NAME, i + 1) >= 0:
        return None, "type name not found exactly once"
    td = off2rva(secs, i - 0x10)
    cols = []
    for name, va, vsize, raw, rsize in secs:
        if name != ".rdata":
            continue
        blob = data[raw:raw + rsize]
        j = blob.find(struct.pack("<I", td))
        while j >= 0:
            c = j - 0x0C
            if c >= 0 and struct.unpack_from("<I", blob, c)[0] == 1 and struct.unpack_from("<I", blob, c + 4)[0] == 0 \
                    and struct.unpack_from("<I", blob, c + 0x14)[0] == va + c:
                cols.append(va + c)
            j = blob.find(struct.pack("<I", td), j + 1)
    if len(cols) != 1:
        return None, "%d object locators" % len(cols)
    key = struct.pack("<Q", base + cols[0])
    hits = []
    for name, va, vsize, raw, rsize in secs:
        if name != ".rdata":
            continue
        blob = data[raw:raw + rsize]
        j = blob.find(key)
        while j >= 0:
            hits.append(va + j + 8)
            j = blob.find(key, j + 1)
    if len(hits) != 1:
        return None, "%d vtables" % len(hits)
    return hits[0], None


def slot(data, base, secs, vt, n):
    return struct.unpack_from("<Q", data, rva2off(secs, vt + 8 * n))[0] - base


def code(data, secs, rva, n):
    o = rva2off(secs, rva)
    return data[o:o + n]


def has(b, pat):
    """pat: bytes with None as a wildcard; True if found anywhere in b"""
    for i in range(len(b) - len(pat) + 1):
        if all(p is None or b[i + k] == p for k, p in enumerate(pat)):
            return i
    return -1


def check(path):
    data, base, secs = load(path)
    vt, err = find_vtable(data, base, secs)
    print("%s" % os.path.basename(path))
    if vt is None:
        print("  FAIL eMoveTask vtable: %s" % err)
        return False
    s13, s14, s4, s0 = (slot(data, base, secs, vt, n) for n in (13, 14, 4, 0))
    print("  eMoveTask vtable exe+0x%X | slot 13 exe+0x%X, slot 14 exe+0x%X, slot 4 exe+0x%X, slot 0 exe+0x%X" % (vt, s13, s14, s4, s0))
    ok = True
    c13 = code(data, secs, s13, 0x20)
    a = has(c13, [0xF7, 0x41, 0x18, 0x00, 0x00, 0x01, 0x00])                     # test dword ptr [rcx+18h], 10000h
    print("  slot 13: test dword ptr [rcx+18h], 10000h at +0x%X  %s" % (a, "ok" if 0 <= a <= 0x10 else "MISSING"))
    ok &= 0 <= a <= 0x10
    c14 = code(data, secs, s14, 0xC0)
    b = has(c14, [0xFF, 0x50, 0x68])                                             # call qword ptr [rax+68h]  (slot 13 on itself)
    c = -1                                                                       # mov r32,[r+3Ch]; sub r32,3; je; cmp r32,1; je
    for i in range(len(c14) - 12):
        if (c14[i] == 0x8B and c14[i + 2] == 0x3C and c14[i + 3] == 0x83 and (c14[i + 4] & 0xF8) == 0xE8 and c14[i + 5] == 3 and c14[i + 6] == 0x74
                and c14[i + 8] == 0x83 and (c14[i + 9] & 0xF8) == 0xF8 and c14[i + 10] == 1 and c14[i + 11] == 0x74):
            c = i
            break
    print("  slot 14: call [rax+68h] at +0x%X, state test [this+3Ch] in {3,4} at +0x%X  %s" % (b, c, "ok" if 0 <= b < c else "MISSING"))
    ok &= 0 <= b < c
    c4 = code(data, secs, s4, 0x20)
    d = has(c4, [0x8B, 0x41, 0x18])                                              # mov eax, [rcx+18h]
    e = has(c4, [0x0F, 0xBA, 0xE0, 0x10])                                        # bt eax, 10h
    print("  slot 4: mov eax,[rcx+18h] at +0x%X, bt eax,10h at +0x%X  %s" % (d, e, "ok" if 0 <= d < e <= 0x18 else "MISSING"))
    ok &= 0 <= d < e <= 0x18
    return ok


def main():
    folder = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_BIN
    good = True
    for n in EXES:
        good &= check(os.path.join(folder, n))
    print("SMOOTH CHECK OK" if good else "SMOOTH CHECK FAILED")
    return 0 if good else 1


if __name__ == "__main__":
    sys.exit(main())
