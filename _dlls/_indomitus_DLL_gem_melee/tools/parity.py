"""gem_melee T3 parity tool (spec section 8.5).

usage: python tools\\parity.py
           P-1 reference runs of the three ORIGINAL test binaries == the baselines
           P-2 / P-2b the merged module tests (gem_melee_test.exe --stance / --pen / --chase / --linked) == the references
           P-3 install dry run (gem_melee_itest.exe --install) == the patch set derived from the reference texts + the exe file bytes
           P-3c the same attach with the attach batch (--attach-time <exe> batch): the 13 code writes share one thread walk
           P-4 every engine address of the live editor logs (payload a1d6ed3c) appears on the editor dry run's module lines
           P-5 out\\dll\\gem_melee.dll: exports {gem_quant_listen}, imports KERNEL32 + the CRT only, a TLS directory
           P-5b a copy of out\\dll\\gem_melee.dll loaded into child python processes: banner, modules line, early and late guard
           P-7 every compile / link line of every out\\obj\\*\\commands.txt is exactly the spec 1.3 command of that object
       python tools\\parity.py --live <gem_melee_editor.log> [<gem_teleport.log>]
           P-6 (T4, run by the main session after an editor start through the test launcher): P-4 against the real log plus the live
           lines (tick hooked, 26 patches, the H2 runtime patch, one thread walk for the 13 code writes, link clock check, no WARNING /
           NOT installed / OFF / FAILED line except
           the server-only manager line, the crash logger ON, a stance hb: line; with the Teleport log: stance's 'orders: listening' line
           and Teleport's registration names gem_melee.dll)
Writes out\\parity\\parity_report.txt (--live: parity_live_report.txt, so a live check never replaces the offline report) and the raw
outputs next to it; last line PARITY OK or PARITY FAILED (<n>). Nothing is written
into inputs\\, probe\\ or baseline\\ (the reference exes run with out\\parity\\ as their working folder; P-1 compares the folder
listings before and after).
Needs `python tools\\build.py dll test` first (out\\dll\\gem_melee.dll, out\\test\\gem_melee_test.exe, out\\test\\gem_melee_itest.exe).
"""
import os
import re
import struct
import subprocess
import sys
from pathlib import Path

GM = Path(__file__).resolve().parents[1]
OUT = GM / "out" / "parity"
DEFAULT_BIN = "C:/Program Files (x86)/Steam/steamapps/common/Call to Arms - Gates of Hell/binaries/x64"
EXE_NAMES = ["call_to_arms.exe", "call_to_arms_ed.exe", "call_to_arms_server.exe"]
LIVE = GM / "inputs" / "live_payload_a1d6ed3c"
LIVE_RANGES = [("stance", "gem_meleestance_editor.log", 13), ("chase", "gem_meleechase.log", 26), ("pen", "gem_penfilter_editor.log", 15)]
BASE_OPTS = ["-mcpu=baseline", "-O2", "-ffp-contract=off", "-fno-fast-math"]
TIMEOUT = 1800
ADDR = re.compile(r"exe\+0x([0-9A-Fa-f]+)")


class Report:
    def __init__(self):
        self.lines = []
        self.fails = 0

    def say(self, s=""):
        print(s)
        sys.stdout.flush()
        self.lines.append(s)

    def check(self, ok, what, detail=""):
        self.say(f"  {'OK  ' if ok else 'FAIL'} {what}{(': ' + detail) if detail and not ok else ''}")
        if not ok:
            self.fails += 1
        return ok

    def write(self, name="parity_report.txt"):
        OUT.mkdir(parents=True, exist_ok=True)
        (OUT / name).write_text("\n".join(self.lines) + "\n", encoding="utf-8")


def exe_args():
    b = os.environ.get("GOH_BIN", "").strip() or DEFAULT_BIN
    b = b.replace("\\", "/").rstrip("/")
    return [f"{b}/{n}" for n in EXE_NAMES]


def lf(b):
    return b.replace(b"\r\n", b"\n")


def run(exe, args, name):
    """run with out\\parity as the working folder; stdout (bytes, LF) saved as out\\parity\\<name>"""
    p = subprocess.run([str(exe), *[str(a) for a in args]], cwd=OUT, capture_output=True, timeout=TIMEOUT)
    out = lf(p.stdout)
    (OUT / name).write_bytes(out)
    if p.stderr:
        (OUT / (name + ".stderr")).write_bytes(p.stderr)
    return p.returncode, out


def first_diff(a, b):
    al, bl = a.split(b"\n"), b.split(b"\n")
    for i in range(max(len(al), len(bl))):
        x = al[i] if i < len(al) else b"<end>"
        y = bl[i] if i < len(bl) else b"<end>"
        if x != y:
            return f"line {i + 1}: got {x.decode('latin-1')[:160]!r} want {y.decode('latin-1')[:160]!r}"
    return ""


def listing(folder):
    out = {}
    for root, _, files in os.walk(folder):
        for f in files:
            p = os.path.join(root, f)
            st = os.stat(p)
            out[os.path.relpath(p, folder)] = (st.st_size, st.st_mtime_ns)
    return out


# ---------------------------------------------------------------------------------------------------------------- PE reading
class PE:
    def __init__(self, path):
        self.d = Path(path).read_bytes()
        d = self.d
        pe = struct.unpack_from("<I", d, 0x3C)[0]
        if d[pe:pe + 4] != b"PE\0\0":
            raise ValueError(f"{path}: not a PE file")
        nsec, = struct.unpack_from("<H", d, pe + 6)
        optsz, = struct.unpack_from("<H", d, pe + 20)
        self.opt = pe + 24
        if struct.unpack_from("<H", d, self.opt)[0] != 0x20B:
            raise ValueError(f"{path}: not PE32+")
        self.secs = []
        for i in range(nsec):
            s = self.opt + optsz + 40 * i
            vsz, va, rsz, roff = struct.unpack_from("<IIII", d, s + 8)
            self.secs.append((va, vsz, rsz, roff))

    def datadir(self, i):
        return struct.unpack_from("<II", self.d, self.opt + 112 + 8 * i)

    def off(self, rva):
        for va, vsz, rsz, ro in self.secs:
            if va <= rva < va + max(vsz, rsz):
                if rva - va >= rsz:
                    return None                                   # inside the section but not in the file (zero fill)
                return rva - va + ro
        return None

    def bytes_at(self, rva, n):
        o = self.off(rva)
        return None if o is None else self.d[o:o + n]

    def cstr(self, rva):
        o = self.off(rva)
        return self.d[o:self.d.index(b"\0", o)].decode("ascii", "replace")

    def exports(self):
        erva, _ = self.datadir(0)
        if not erva:
            return set()
        e = self.off(erva)
        nnames, = struct.unpack_from("<I", self.d, e + 24)
        names_rva, = struct.unpack_from("<I", self.d, e + 32)
        out = set()
        for i in range(nnames):
            nr, = struct.unpack_from("<I", self.d, self.off(names_rva) + 4 * i)
            out.add(self.cstr(nr))
        return out

    def imports(self):
        irva, _ = self.datadir(1)
        out = []
        if not irva:
            return out
        o = self.off(irva)
        while True:
            name_rva, = struct.unpack_from("<I", self.d, o + 12)
            if not name_rva:
                break
            out.append(self.cstr(name_rva))
            o += 20
        return out


# ---------------------------------------------------------------------------------------------------------------- reference text parsers
def stance_blocks(text):
    """ms_test output: one block per exe path line"""
    blocks, cur = {}, None
    for l in text.split("\n"):
        m = re.match(r"^\S.*/(call_to_arms[^/]*\.exe)$", l)
        if m:
            cur = m.group(1)
            blocks[cur] = []
            continue
        if cur and l.startswith("  "):
            blocks[cur].append(l)
        elif cur and not l.startswith(" "):
            cur = None
    return blocks


def pen_blocks(text):
    blocks, cur = {}, None
    for l in text.split("\n"):
        m = re.match(r"^== .*/(call_to_arms[^/]*\.exe)$", l)
        if m:
            cur = m.group(1)
            blocks[cur] = []
            continue
        if cur and l.startswith("  "):
            blocks[cur].append(l)
        elif cur:
            cur = None
    return blocks


def h(s):
    return int(s, 16)


def le64(x):
    return struct.pack("<Q", x).hex().upper()


def expected_patches(exe, st_block, pen_block, chase_text, base):
    """[(mod, name, rva, len, orig-check, new-check)] derived from the P-1 texts; checks are functions(orig_bytes, new_bytes) -> str|None"""
    exp = []
    st = "\n".join(st_block)
    m = re.search(r"ammo match exe\+0x([0-9A-F]+)", st)
    h1 = h(m.group(1))
    prolog = bytes.fromhex("48895C2420555657415441554156")
    exp.append(("stance", "H1 MatchAmmo prologue", h1, 14,
                lambda o, n: None if o == prolog and n[:6] == bytes.fromhex("FF2500000000") else "orig / new bytes"))
    m = re.search(r"ExecuteInstant exe\+0x([0-9A-F]+) in (\d+) vtables", st)
    ei, nvt = h(m.group(1)), int(m.group(2))
    exp.append(("stance", "H3 ExecuteInstant slot", None, 8,
                lambda o, n, ei=ei: None if o == struct.pack("<Q", base + ei) else f"orig {o.hex()} != base + ExecuteInstant", nvt))
    m = re.search(r"blow reach: table exe\+0x([0-9A-F]+) = 17/29/35", st)
    exp.append(("stance", "H4 blow reach table", h(m.group(1)), 12,
                lambda o, n: None if o == struct.pack("<3f", 17, 29, 35) and n == struct.pack("<3f", 45, 45, 45) else "orig / new floats"))
    # chase: 3 VT lines (PTR) and 6 CALL / JMP lines (CODE)
    for m in re.finditer(r"^\s+VT\s+.*?slot exe\+0x([0-9A-F]+) = exe\+0x([0-9A-F]+)\s*$", chase_text, re.M):
        fn = h(m.group(2))
        exp.append(("chase", None, h(m.group(1)), 8, lambda o, n, fn=fn: None if o == struct.pack("<Q", base + fn) else "orig != base + fn"))
    for m in re.finditer(r"^\s+(?:CALL|JMP)\s+.*?exe\+0x([0-9A-F]+) : ((?:[0-9A-F]{2} ){4}[0-9A-F]{2})\s*$", chase_text, re.M):
        ob = bytes.fromhex(m.group(2).replace(" ", ""))
        exp.append(("chase", None, h(m.group(1)), 5, lambda o, n, ob=ob: None if o == ob and n[:1] == ob[:1] else "orig bytes / opcode"))
    # pen
    pb = "\n".join(pen_block)
    m = re.search(r"ctx builder exe\+0x([0-9A-F]+)", pb)
    exp.append(("pen", "W ctx builder prologue", h(m.group(1)), 15,
                lambda o, n: None if n[:6] == bytes.fromhex("FF2500000000") and n[14:15] == b"\x90" else "new bytes"))
    m = re.search(r"volume site exe\+0x([0-9A-F]+), no-volume site exe\+0x([0-9A-F]+) \(match \+0x([0-9A-F]+), next \+0x[0-9A-F]+\), "
                  r"tag test exe\+0x([0-9A-F]+)", pb)
    vs, nvs, match, tagt = (h(m.group(i)) for i in (1, 2, 3, 4))

    def rel(site, ilen, o):
        return site + ilen + struct.unpack_from("<i", o, ilen - 4)[0]
    exp.append(("pen", "A1 volume tag test call", vs, 5,
                lambda o, n, s=vs, t=tagt: None if o[:1] == b"\xE8" and rel(s, 5, o) == t else "orig is not a call to the tag test"))
    exp.append(("pen", "A2 no-volume row test", nvs, 8,
                lambda o, n, s=nvs, t=match: None if o[:4] == bytes.fromhex("A8020F84") and rel(s, 8, o) == t and n[:1] == b"\xE9"
                and n[5:] == b"\x90\x90\x90" else "orig / new bytes"))
    m = re.search(r"picker solver call exe\+0x([0-9A-F]+) -> solver exe\+0x([0-9A-F]+)", pb)
    sc, sol = h(m.group(1)), h(m.group(2))
    exp.append(("pen", "B solver call", sc, 5, lambda o, n, s=sc, t=sol: None if o[:1] == b"\xE8" and rel(s, 5, o) == t else "orig is not a call to the solver"))
    m = re.search(r"picker try-loop head exe\+0x([0-9A-F]+) \(candidates exe\+0x([0-9A-F]+)\.\.\+8\)", pb)
    ph, cand = h(m.group(1)), h(m.group(2))
    exp.append(("pen", "P try-loop head", ph, 7,
                lambda o, n, s=ph, t=cand + 8: None if o[:3] == bytes.fromhex("488B3D") and rel(s, 7, o) == t and n[:1] == b"\xE9"
                and n[5:] == b"\x90\x90" else "orig / new bytes"))
    m = re.search(r"exhausted clear call exe\+0x([0-9A-F]+) -> clear exe\+0x([0-9A-F]+)", pb)
    xc, clr = h(m.group(1)), h(m.group(2))
    exp.append(("pen", "X exhausted clear call", xc, 5, lambda o, n, s=xc, t=clr: None if o[:1] == b"\xE8" and rel(s, 5, o) == t else "orig is not a call to clear"))
    return exp


def parse_install(text):
    base, patches, addr = None, [], {}
    for l in text.split("\n"):
        if l.startswith("BASE|"):
            f = l.split("|")
            base = (h(f[1]), f[2])
        elif l.startswith("PATCH|"):
            f = l.split("|")
            patches.append({"mod": f[1], "name": f[2], "rva": h(f[3]) if not f[3].startswith("abs:") and f[3] != "NULL" else None,
                            "len": int(f[4]), "orig": bytes.fromhex(f[5]), "new": bytes.fromhex(f[6]), "state": f[7]})
        elif l.startswith("ADDR|"):
            f = l.split("|")
            addr[f[1]] = f[2]
    return base, patches, addr


def p3_one(r, exe_path, inst_text, st_block, pen_block, chase_text):
    name = exe_path.rsplit("/", 1)[-1]
    base, patches, addr = parse_install(inst_text)
    if not base:
        return r.check(False, f"P-3 {name}: no BASE line")
    exe = PE(exe_path)
    exp = expected_patches(name, st_block, pen_block, chase_text, base[0])
    used = set()
    ok = True
    for e in exp:
        mod, nm, rva, ln, chk = e[0], e[1], e[2], e[3], e[4]
        count = e[5] if len(e) > 5 else 1
        cand = [i for i, p in enumerate(patches) if i not in used and p["mod"] == mod and (nm is None or p["name"] == nm)
                and (rva is None or p["rva"] == rva)]
        if len(cand) < count:
            ok = r.check(False, f"P-3 {name}: {mod} {nm or ''} exe+0x{rva or 0:X} x{count}", f"found {len(cand)}") and ok
            continue
        for i in cand[:count]:
            p = patches[i]
            used.add(i)
            why = []
            if p["len"] != ln:
                why.append(f"len {p['len']} != {ln}")
            if p["state"] != "WRITTEN":
                why.append(p["state"])
            c = chk(p["orig"], p["new"])
            if c:
                why.append(c)
            is_ptr = (mod == "chase" and ln == 8) or p["name"] == "H3 ExecuteInstant slot"
            if not is_ptr:                                         # CODE / DATA: orig = the exe file's bytes (PTR slots: checked against base + fn)
                fb = exe.bytes_at(p["rva"], p["len"])
                if fb is None or fb != p["orig"]:
                    why.append(f"orig != file bytes {fb.hex() if fb else None}")
            if why:
                ok = r.check(False, f"P-3 {name}: {p['mod']} {p['name']} exe+0x{p['rva']:X}", "; ".join(why)) and ok
    # gem_melee 0.1.14: the two hull-melee call redirects of the chase module (no counterpart in the three original dlls): none or
    # both (the selection code is recognised on the editor exe only so far), each a 5-byte call redirect over the file's own bytes
    hull_names = ("melee weapon selection reach test", "melee range in the target evaluation", "melee range against a target",
                  "melee aim status slot", "melee fire line slot",   # 0.1.17: 5 + 5 aimer vtable slots (pointer writes)
                  "melee shot range slot")                           # 0.1.18: 3 shooter vtable slots
    hull = [p for i, p in enumerate(patches) if i not in used and p["mod"] == "chase" and p["name"] in hull_names]
    for p in hull:
        fb = exe.bytes_at(p["rva"], p["len"])
        if p["name"].endswith(" slot"):
            good = p["len"] == 8 and p["state"] == "WRITTEN"
            ok = r.check(good, f"P-3 {name}: gem_melee's own entry chase {p['name']} exe+0x{p['rva']:X}: a vtable slot write") and ok
            continue
        good = p["len"] == 5 and p["state"] == "WRITTEN" and fb is not None and fb == p["orig"] and fb[:1] == b"\xE8" and p["new"][:1] == b"\xE8"
        ok = r.check(good, f"P-3 {name}: gem_melee's own entry chase {p['name']} exe+0x{p['rva']:X}: a call redirect over the file's bytes") and ok
    ok = r.check(len(hull) in (19, 21), f"P-3 {name}: hull-melee entries: {len(hull)} (6 range call sites, 10 aimer slots, 3 shooter slots, + the 2 selection hooks on the editor exe)") and ok
    extra = [p for i, p in enumerate(patches) if i not in used and p not in hull]
    for p in extra:
        ok = r.check(False, f"P-3 {name}: unexpected entry {p['mod']} {p['name']} exe+0x{p['rva'] or 0:X} {p['state']}") and ok
    bad = [p for p in patches if p["state"] != "WRITTEN"]
    # review F7: the install order inside the dll is chase, stance, pen (spec 1.1); the registry lists entries in write order
    rank = {"chase": 0, "stance": 1, "pen": 2}
    seq = [rank.get(p["mod"], 9) for p in patches]
    ok = r.check(seq == sorted(seq), f"P-3 {name}: registry entries in install order chase, stance, pen",
                 " ".join(p["mod"] for p in patches)) and ok
    n_exp = sum(e[5] if len(e) > 5 else 1 for e in exp)
    ok = r.check(len(patches) == 26 + len(hull) and n_exp == 26 and not bad and not extra and ok,
                 f"P-3 {name}: {len(patches)} entries (expected {n_exp} from the references, spec 26, + {len(hull)} hull-melee), all WRITTEN, base {base[0]:X} {base[1]}",
                 f"{len(bad)} not written, {len(extra)} unexpected") and ok
    # addresses: clocks, cease-fire / stop, MatchAmmo against the reference texts
    st = "\n".join(st_block)
    pb = "\n".join(pen_block)
    clk = re.search(r"clock exe\+0x([0-9A-F]+)\s*$", st, re.M).group(1)
    pclk = re.search(r"sim clock exe\+0x([0-9A-F]+)", pb).group(1)
    cclk = re.search(r"clock exe\+0x([0-9A-F]+)\s*$", re.search(r"vehicle: approach call.*", chase_text).group(0)).group(1)
    stop = re.search(r"weapon stop exe\+0x([0-9A-F]+)", pb).group(1)
    clear = re.search(r"clear target exe\+0x([0-9A-F]+)", chase_text).group(1)
    ma = re.search(r"MatchAmmo exe\+0x([0-9A-F]+)", pb).group(1)
    want = {"core_clock": clk, "stance_clock": clk, "chase_clock": cclk, "pen_clock": pclk, "chase_ceasefire": clear, "pen_stop": stop,
            "pen_matchammo": ma}
    diff = [f"{k} {addr.get(k)} != {v}" for k, v in want.items() if addr.get(k, "").upper() != v.upper()]
    r.check(not diff and clk == pclk == cclk and clear == stop, f"P-3 {name}: ADDR lines = the references (clock exe+0x{clk}, stop exe+0x{stop}, "
            f"MatchAmmo exe+0x{ma})", "; ".join(diff))
    r.check(inst_text.rstrip().endswith("INSTALL OK"), f"P-3 {name}: last line INSTALL OK")
    return ok


def p3c(r, exes):
    """STARTUP_batch: the attach batch around the module attaches. gem_melee_itest --attach-time <exe> batch runs core_attach_all with
    the batch the real DllMain opens (dry 2, asked for explicitly; the plain dry run of P-3 has none): every code write of the three
    modules must share ONE thread walk"""
    itest = GM / "out" / "test" / "gem_melee_itest.exe"
    want = re.compile(r"code writes: (19|21) with the other threads suspended, 1 thread walk \(attach batch, ")   # 13 + the 6 range call sites (0.1.16) + the two selection hooks on the editor exe (0.1.14)
    for x in exes:
        nm = x.rsplit("/", 1)[-1]
        rc, o = run(itest, ["--attach-time", x, "batch"], f"attach_batch_{nm}.txt")
        line = next((l for l in o.decode("latin-1").split("\n") if l.startswith("TIME|")), "")
        f = line.split("|")
        ok = rc == 0 and len(f) >= 6 and f[4] == "all ON" and want.match(f[5]) is not None
        r.check(ok, f"P-3c {nm}: with the attach batch the 13 code writes share one thread walk", f"exit {rc}; {line[:300]}")


def p4(r, inst_text, live_dir, live_text=None):
    """every exe+0x token of the live editor logs' install lines appears on the matching module lines of the given (dry run or live) log"""
    mod_tokens = {}
    for l in inst_text.split("\n"):
        m = re.search(r"\[(stance|chase|pen)\] ", l)
        if m:
            mod_tokens.setdefault(m.group(1), set()).update(t.upper() for t in ADDR.findall(l))
    all_ok = True
    for mod, fn, n in LIVE_RANGES:
        lines = Path(live_dir, fn).read_text(encoding="latin-1").split("\n")[:n]
        toks = sorted({t.upper() for l in lines for t in ADDR.findall(l)}, key=lambda x: int(x, 16))
        missing = [t for t in toks if t not in mod_tokens.get(mod, set())]
        all_ok &= r.check(bool(toks) and not missing, f"P-4 {mod}: {len(toks)} addresses of {fn} lines 1-{n} on the [{mod}] lines",
                          ("missing exe+0x" + ", exe+0x".join(missing)) if missing else "no address in the old log lines")
    return all_ok


def p5(r):
    dll = GM / "out" / "dll" / "gem_melee.dll"
    if not dll.exists():
        return r.check(False, "P-5 out\\dll\\gem_melee.dll exists (run build.py dll)")
    pe = PE(dll)
    ex = pe.exports()
    r.check(ex == {"gem_quant_listen"}, f"P-5 exports {sorted(ex)} == ['gem_quant_listen']")
    imps = pe.imports()
    crt = re.compile(r"^(msvcrt\.dll|ucrtbase\.dll|api-ms-win-crt-[a-z0-9-]+\.dll)$", re.I)
    bad = [i for i in imps if i.lower() != "kernel32.dll" and not crt.match(i)]
    r.check(not bad and any(i.lower() == "kernel32.dll" for i in imps), f"P-5 imports {imps}: KERNEL32 + the CRT only", f"other: {bad}")
    tls = pe.datadir(9)
    r.check(tls[0] != 0 and tls[1] != 0, f"P-5 TLS directory rva 0x{tls[0]:X} size 0x{tls[1]:X}")


LOADCHILD = r'''
import ctypes, shutil, sys, time
from pathlib import Path
mode, d, dll, ini, dummy = sys.argv[1], Path(sys.argv[2]), sys.argv[3], sys.argv[4], sys.argv[5]
d.mkdir(parents=True, exist_ok=True)
for f in d.glob("*"):
    f.unlink()
shutil.copy2(dll, d / "gem_melee.dll"); shutil.copy2(ini, d / "gem_melee.ini"); shutil.copy2(dummy, d / "gem_penfilter.dll")
if mode == "early":
    ctypes.WinDLL(str(d / "gem_penfilter.dll"))
ctypes.WinDLL(str(d / "gem_melee.dll"))
if mode == "late":
    time.sleep(0.3)
    ctypes.WinDLL(str(d / "gem_penfilter.dll"))
log = d / "gem_melee.log"
for _ in range(60):                                          # the late guard looks once a wall second
    time.sleep(0.1)
    t = log.read_text(encoding="latin-1") if log.exists() else ""
    if mode != "late" or "was loaded after" in t:
        break
time.sleep(0.3)
print(log.read_text(encoding="latin-1") if log.exists() else "(no log)")
'''
LOADWANT = {
    "plain": [r"\[core\] GEM Melee 0\.1\.20 \(stance 0\.7\.42, chase 0\.5\.4, pen 0\.1\.1\) loaded into python[^ ]*\.exe \(timestamp [0-9A-F]{8}\), ini .*gem_melee\.ini found$",
              r"\[core\] chase module off: python[^ ]*\.exe is not a call_to_arms exe$",
              r"\[core\] modules: chase OFF \(not a call_to_arms exe\), stance (ON|OFF), pen (ON|OFF) \| background thread ON \(",
              r"\[link\] R4 flip detector: ",
              # STARTUP_batch: the real attach (DllMain) opens the attach batch (python.exe gives no code write, so no walk is needed)
              r"\[core\] code writes: none \(attach batch open, no thread walk needed\)$"],
    "early": [r"\[core\] gem_penfilter\.dll is already loaded in this process: the pen module stays off \(remove one of them\)$",
              r"\[core\] modules: .*, pen OFF \(gem_penfilter\.dll already loaded\) \|"],
    "late": [r"\[core\] WARNING: gem_penfilter\.dll was loaded after gem_melee\.dll: remove it from the payload$"],
}
LOADNOT = {"plain": [r"crash log +ON", r"WARNING"], "early": [], "late": [r"is already loaded in this process"]}


def p5b(r):
    """review F2 / WP4_verify M1: the SHIPPED dll's DllMain. A copy of out\\dll\\gem_melee.dll + its ini is loaded into child python
    processes (never a game exe), alone, after and before an empty dll named gem_penfilter.dll; the log must show the banner, the
    modules line, the attach batch of the real attach (STARTUP_batch), the early guard and the late guard (written only by the core
    background thread, so it proves the thread runs).
    python.exe is not a call_to_arms exe: chase off, no crash logger (MeleeChase's exe gate, review RP-4)."""
    dll = GM / "out" / "dll" / "gem_melee.dll"
    ini = GM / "out" / "dll" / "gem_melee.ini"
    dummy = GM / "out" / "test" / "dummy_old.dll"
    if not (dll.exists() and ini.exists() and dummy.exists()):
        return r.check(False, "P-5b out\\dll\\gem_melee.dll, gem_melee.ini and out\\test\\dummy_old.dll exist (run build.py dll test)")
    for mode in ("plain", "early", "late"):
        d = OUT / f"load_{mode}"
        p = subprocess.run([sys.executable, "-c", LOADCHILD, mode, str(d), str(dll), str(ini), str(dummy)], capture_output=True, timeout=120)
        t = lf(p.stdout).decode("latin-1")
        (OUT / f"load_{mode}.txt").write_text(t, encoding="latin-1")
        miss = [w for w in LOADWANT[mode] if not re.search(w, t, re.M)]
        extra = [w for w in LOADNOT[mode] if re.search(w, t, re.M)]
        r.check(p.returncode == 0 and not miss and not extra, f"P-5b load test '{mode}': a copy of the shipped dll loaded into python.exe "
                f"({len(t.splitlines())} log lines, out\\parity\\load_{mode}.txt)",
                f"exit {p.returncode}; missing {miss}; unexpected {extra}; {p.stderr.decode('latin-1')[-300:]}")


# spec 1.3: the flags of every object (the -o file's name without .o / _sa.o) and its source; the link line (review F5)
P7_BASE = ["-target", "x86_64-windows-gnu", "-mcpu=baseline", "-O2", "-ffp-contract=off", "-fno-fast-math"]
P7_NEW = ["-Wall", "-Wextra", "-Wno-unused-parameter", "-Wno-missing-field-initializers"]
P7_LINK = ["-target", "x86_64-windows-gnu", "-mcpu=baseline", "-O2"]
P7_OBJ = {
    "core": ("src\\core\\core.c", P7_BASE + P7_NEW), "core_patch": ("src\\core\\core_patch.c", P7_BASE + P7_NEW),
    "core_crash": ("src\\core\\core_crash.c", P7_BASE + P7_NEW), "core_dllmain": ("src\\core\\core_dllmain.c", P7_BASE + P7_NEW),
    "link": ("src\\link\\link.c", P7_BASE + P7_NEW), "stance": ("src\\stance\\stance.c", P7_BASE),
    "stance_t": ("src\\stance\\stance.c", P7_BASE + ["-DMS_TEST", "-Dmain=stance_test_main"]), "chase": ("src\\chase\\chase.c", P7_BASE + P7_NEW),
    "pen": ("src\\pen\\pen.c", P7_BASE), "pen_t": ("src\\pen\\pen.c", P7_BASE + ["-DPF_TEST", "-Dmain=pen_test_main"]),
    "test_chase": ("src\\test\\test_chase.c", P7_BASE), "test_chase_sa": ("src\\test\\test_chase.c", P7_BASE + ["-DTEST_CHASE_STANDALONE"]),
    "test_core": ("src\\test\\test_core.c", P7_BASE + P7_NEW), "test_core_sa": ("src\\test\\test_core.c", P7_BASE + P7_NEW + ["-DTEST_CORE_STANDALONE"]),
    "test_main": ("src\\test\\test_main.c", P7_BASE + P7_NEW), "test_itest": ("src\\test\\test_itest.c", P7_BASE + P7_NEW),
    "test_linked": ("src\\test\\test_linked.c", P7_BASE + P7_NEW), "main_stance": ("src\\test\\main_stance.c", P7_BASE + P7_NEW),
    "main_pen": ("src\\test\\main_pen.c", P7_BASE + P7_NEW), "main_link_stub": ("src\\test\\main_link_stub.c", P7_BASE + P7_NEW),
    "dummy_old": ("src\\test\\dummy_old.c", P7_BASE + P7_NEW),
}


def argv_of(line):
    """the exact argument vector of a commands.txt line (written by subprocess.list2cmdline): CommandLineToArgvW"""
    import ctypes
    import ctypes.wintypes as wt
    f = ctypes.windll.shell32.CommandLineToArgvW
    f.restype = ctypes.POINTER(wt.LPWSTR)
    n = ctypes.c_int(0)
    a = f(line, ctypes.byref(n))
    out = [a[i] for i in range(n.value)]
    ctypes.windll.kernel32.LocalFree(a)
    return out


def p7(r):
    """every compile command = zig cc -c <exactly the spec 1.3 flags of that object> <its source> -o <obj>; every link command =
    zig cc <LINK> [-shared -s] <objs> -lkernel32 -o <out> (review F5: a presence test let -ffast-math and a changed link line pass)"""
    n, nl, bad = 0, 0, []
    for cf in sorted((GM / "out" / "obj").glob("*/commands.txt")):
        for l in cf.read_text(encoding="utf-8", errors="replace").splitlines():
            if not l.strip() or l.startswith("#"):
                continue
            a = argv_of(l)
            if len(a) < 3 or not a[0].lower().endswith("zig.exe") or a[1] != "cc":
                bad.append(f"{cf.parent.name}: not a zig cc line: {l[:100]}")
                continue
            if a[2] == "-c":
                n += 1
                if len(a) < 6 or a[-2] != "-o" or not a[-1].endswith(".o"):
                    bad.append(f"{cf.parent.name}: compile line shape: {l[:100]}")
                    continue
                obj = Path(a[-1]).stem
                if obj not in P7_OBJ:
                    bad.append(f"{cf.parent.name}: object {obj} is not in the spec 1.3 table")
                    continue
                src, flags = P7_OBJ[obj]
                if a[-3] != src or a[3:-3] != flags:
                    bad.append(f"{cf.parent.name}: {obj}: got {a[-3]} {' '.join(a[3:-3])}, want {src} {' '.join(flags)}")
            else:
                nl += 1
                k = 2 + len(P7_LINK)
                dll = a[-1].lower().endswith(".dll")
                head = P7_LINK + (["-shared", "-s"] if dll else [])
                objs = a[2 + len(head):-3]
                if a[2:2 + len(head)] != head or a[-3:-1] != ["-lkernel32", "-o"] or not objs or not all(o.endswith(".o") for o in objs) or k > len(a):
                    bad.append(f"{cf.parent.name}: link line: {l[:160]}")
    r.check(n > 0 and nl > 0 and not bad, f"P-7 {n} compile lines and {nl} link lines in out\\obj\\*\\commands.txt are exactly the spec 1.3 commands "
            f"(compile: {' '.join(BASE_OPTS)} and nothing that contradicts them)", "; ".join(bad[:5]))


# ---------------------------------------------------------------------------------------------------------------- main modes
def offline(r):
    test = GM / "out" / "test" / "gem_melee_test.exe"
    itest = GM / "out" / "test" / "gem_melee_itest.exe"
    if not test.exists() or not itest.exists():
        r.check(False, "out\\test\\gem_melee_test.exe and gem_melee_itest.exe exist (run build.py test)")
        return
    exes = exe_args()
    names = [x.rsplit("/", 1)[-1] for x in exes]
    # ---- P-1
    r.say("P-1 references (the original test binaries)")
    before = (listing(GM / "inputs"), listing(GM / "probe"), listing(GM / "baseline"))
    refs = {}
    rc, refs["stance"] = run(GM / "inputs" / "meleestance_0.7.42" / "dist" / "ms_test.exe", exes, "ref_stance.txt")
    base_st = lf((GM / "baseline" / "ms_test_0.7.42.txt").read_bytes())
    r.check(rc == 0 and refs["stance"] == base_st, "P-1 ms_test.exe g e s == baseline\\ms_test_0.7.42.txt", f"exit {rc}; {first_diff(refs['stance'], base_st)}")
    rc, refs["pen"] = run(GM / "inputs" / "penfilter_0.1.1" / "out" / "pf_test.exe", exes, "ref_pen.txt")
    base_pf = lf((GM / "baseline" / "pf_test_0.1.1.txt").read_bytes())
    r.check(rc == 0 and refs["pen"] == base_pf, "P-1 pf_test.exe g e s == baseline\\pf_test_0.1.1.txt", f"exit {rc}; {first_diff(refs['pen'], base_pf)}")
    for x, nm in zip(exes, names):
        rc, refs[nm] = run(GM / "probe" / "chase" / "chase_probe.exe", [x], f"ref_chase_{nm}.txt")
        want = lf((GM / "probe" / "chase" / f"out_{nm}.txt").read_bytes())
        r.check(rc == 0 and refs[nm] == want, f"P-1 chase_probe.exe {nm} == probe\\chase\\out_{nm}.txt", f"exit {rc}; {first_diff(refs[nm], want)}")
    after = (listing(GM / "inputs"), listing(GM / "probe"), listing(GM / "baseline"))
    r.check(before == after, "P-1 nothing written into inputs\\, probe\\ or baseline\\ by the reference runs")
    # ---- P-2
    r.say("P-2 merged module tests (gem_melee_test.exe)")
    rc, o = run(test, ["--stance", *exes], "merged_stance.txt")
    r.check(rc == 0 and o == refs["stance"], "P-2 --stance g e s == P-1 ms_test output", f"exit {rc}; {first_diff(o, refs['stance'])}")
    rc, o = run(test, ["--pen", *exes], "merged_pen.txt")
    r.check(rc == 0 and o == refs["pen"], "P-2 --pen g e s == P-1 pf_test output", f"exit {rc}; {first_diff(o, refs['pen'])}")
    for x, nm in zip(exes, names):
        rc, o = run(test, ["--chase", x], f"merged_chase_{nm}.txt")
        r.check(rc == 0 and o == refs[nm], f"P-2 --chase {nm} == P-1 chase_probe output", f"exit {rc}; {first_diff(o, refs[nm])}")
    rc, o = run(test, ["--linked", *exes], "merged_linked.txt")
    lines = o.rstrip(b"\n").split(b"\n")
    last = lines[-1].decode("latin-1") if lines else ""
    body = b"\n".join(lines[:-1]) + b"\n"
    ok = rc == 0 and body == refs["stance"] + refs["pen"] and re.match(r"^LINKED OK \(notes [1-9]\d*, traces [1-9]\d*\)$", last)
    r.check(bool(ok), f"P-2b --linked g e s == stance reference + pen reference + '{last}'", f"exit {rc}; {first_diff(body, refs['stance'] + refs['pen'])}")
    # ---- P-3
    r.say("P-3 install dry run (gem_melee_itest.exe --install)")
    st_blocks = stance_blocks(refs["stance"].decode("latin-1"))
    pf_blocks = pen_blocks(refs["pen"].decode("latin-1"))
    inst = {}
    for x, nm in zip(exes, names):
        rc, o = run(itest, ["--install", x], f"install_{nm}.txt")
        inst[nm] = o.decode("latin-1")
        r.check(rc == 0, f"P-3 --install {nm}: exit {rc}")
        if nm not in st_blocks or nm not in pf_blocks:
            r.check(False, f"P-3 {nm}: no block for this exe in the stance / pen references")
            continue
        p3_one(r, x, inst[nm], st_blocks[nm], pf_blocks[nm], refs[nm].decode("latin-1"))
    r.say("P-3c the attach batch (gem_melee_itest.exe --attach-time <exe> batch)")
    p3c(r, exes)
    # ---- P-4
    r.say("P-4 live editor addresses (payload a1d6ed3c) in the editor dry run")
    p4(r, inst.get("call_to_arms_ed.exe", ""), LIVE)
    # ---- P-5, P-7
    r.say("P-5 dll exports / imports / TLS")
    p5(r)
    r.say("P-5b the shipped dll's DllMain (load tests in child python processes)")
    p5b(r)
    r.say("P-7 R7 compile and link commands")
    p7(r)


def live(r, log, tp_log=None):
    r.say(f"P-6 live editor log {log}")
    t = Path(log).read_text(encoding="latin-1")
    p4(r, t, LIVE)
    r.check(re.search(r"\[stance\] .*tick: .*vtable slot exe\+0xF381D0 hooked", t) is not None, "P-6 'tick: ... vtable slot exe+0xF381D0 hooked'")
    r.check(re.search(r"\[core\] patches: (26|28|32|34|42|44|45|47) written, 0 refused", t) is not None, "P-6 '[core] patches: 26 (+ hull-melee redirects) written, 0 refused'")
    r.check(re.search(r"\[core\] runtime patch #\d+ stance H2 quant executor slot .* WRITTEN", t) is not None, "P-6 the [core] H2 runtime patch line")
    r.check(re.search(r"\[core\] code writes: (13|15|19|21) with the other threads suspended, 1 thread walk \(attach batch, ", t) is not None,
            "P-6 '[core] code writes: 13 with the other threads suspended, 1 thread walk (attach batch, ...' (STARTUP_batch)")
    r.check(re.search(r"\[link\] clock check: .*: equal\s*$", t, re.M) is not None, "P-6 '[link] clock check ... equal'")
    r.check(re.search(r"\[core\] WARNING", t) is None, "P-6 no '[core] WARNING' line")
    # review F2: what only the running dll writes (crash logger installed; the core thread's stance heartbeat)
    r.check(re.search(r"\[core\] crash log +ON \(fatal exceptions are written here\)", t) is not None, "P-6 '[core] crash log ... ON'")
    r.check(re.search(r"\[stance\] hb: \[", t) is not None, "P-6 at least one '[stance] hb:' line (the core background thread runs stance's step)")
    bad = [l for l in t.split("\n") if re.search(r"NOT installed|\bOFF\b|FAILED", l) and "manual-control manager" not in l]
    r.check(not bad, "P-6 no 'NOT installed' / 'OFF' / 'FAILED' line (except the server-only manager line)", " | ".join(x.strip()[:140] for x in bad[:4]))
    if tp_log:
        r.check(re.search(r"\[stance\] orders: listening to GEM Teleport's GiveOrder hook \(gem_order_listen\)", t) is not None,
                "P-6 '[stance] orders: listening to GEM Teleport's GiveOrder hook' (stance's background step found Teleport)")
        tt = Path(tp_log).read_text(encoding="latin-1")
        regs = re.findall(r"aim: per-quant tick from (\S+) \(gem_quant_listen\) registered", tt)
        r.check(bool(regs) and regs[-1] == "gem_melee.dll", f"P-6 Teleport's last registration line names gem_melee.dll ({regs[-1] if regs else 'none'})",
                "no 'aim: per-quant tick from ... registered' line (it appears after the first jump-pack action is aimed or ordered)" if not regs else regs[-1])


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    r = Report()
    args = sys.argv[1:]
    if args and args[0] == "--live":
        if len(args) < 2:
            print(__doc__)
            return 2
        live(r, args[1], args[2] if len(args) > 2 else None)
    elif args:
        print(__doc__)
        return 2
    else:
        offline(r)
    r.say(f"PARITY FAILED ({r.fails})" if r.fails else "PARITY OK")
    r.write("parity_live_report.txt" if args else "parity_report.txt")
    return 1 if r.fails else 0


if __name__ == "__main__":
    sys.exit(main())
