"""gem_melee build tool (spec section 1.3).

usage: python tools\\build.py <target> [<target> ...]      (build.bat <target> does the same)
targets: core-test test-stance link-stance test-chase link-chase test-pen link-pen dll test run-test parity ini-check port-check all clean

Every compile is `zig cc -c` with BASE (+ NEW where the spec says so); any compiler or linker output line containing
"warning:" fails the target. Each target compiles into out\\obj\\<target>\\ with its own zig local cache
(out\\zigcache\\<target>) and the shared global cache out\\zigcache\\global, and writes every compile / link command it ran to
out\\obj\\<target>\\commands.txt (run commands go to runs.txt). The game exes come from GOH_BIN (a folder, forward slashes)
or the default install, always passed with forward slashes (the baselines were made that way).
"""
import hashlib
import os
import re
import shutil
import struct
import subprocess
import sys
from pathlib import Path

GM = Path(__file__).resolve().parents[1]
OUT = GM / "out"
ZIG = Path(r"C:\Users\alex_\Documents\IndomitusLauncher\zig\zig.exe")
BASE = ["-target", "x86_64-windows-gnu", "-mcpu=baseline", "-O2", "-ffp-contract=off", "-fno-fast-math"]
NEW = ["-Wall", "-Wextra", "-Wno-unused-parameter", "-Wno-missing-field-initializers"]
LINK = ["-target", "x86_64-windows-gnu", "-mcpu=baseline", "-O2"]
DEFAULT_BIN = "C:/Program Files (x86)/Steam/steamapps/common/Call to Arms - Gates of Hell/binaries/x64"
EXE_NAMES = ["call_to_arms.exe", "call_to_arms_ed.exe", "call_to_arms_server.exe"]
RUN_TIMEOUT = 900

# object -> (source relative to GM, flags, who writes the source)
OBJECTS = {
    "core":           ("src/core/core.c",            BASE + NEW, "WP0"),
    "core_patch":     ("src/core/core_patch.c",      BASE + NEW, "WP0"),
    "core_crash":     ("src/core/core_crash.c",      BASE + NEW, "WP0"),
    "core_dllmain":   ("src/core/core_dllmain.c",    BASE + NEW, "WP0"),
    "link":           ("src/link/link.c",            BASE + NEW, "WP4"),
    "stance":         ("src/stance/stance.c",        BASE, "WP1"),
    "stance_t":       ("src/stance/stance.c",        BASE + ["-DMS_TEST", "-Dmain=stance_test_main"], "WP1"),
    "chase":          ("src/chase/chase.c",          BASE + NEW, "WP2"),
    "pen":            ("src/pen/pen.c",              BASE, "WP3"),
    "pen_t":          ("src/pen/pen.c",              BASE + ["-DPF_TEST", "-Dmain=pen_test_main"], "WP3"),
    "test_chase":     ("src/test/test_chase.c",      BASE, "WP2"),
    "test_core":      ("src/test/test_core.c",       BASE + NEW, "WP0"),
    "test_main":      ("src/test/test_main.c",       BASE + NEW, "WP0"),
    "test_itest":     ("src/test/test_itest.c",      BASE + NEW, "WP4"),
    "test_linked":    ("src/test/test_linked.c",     BASE + NEW, "WP4"),
    "main_stance":    ("src/test/main_stance.c",     BASE + NEW, "WP0"),
    "main_pen":       ("src/test/main_pen.c",        BASE + NEW, "WP0"),
    "main_link_stub": ("src/test/main_link_stub.c",  BASE + NEW, "WP0"),
    "dummy_old":      ("src/test/dummy_old.c",       BASE + NEW, "REVIEW F2"),   # P-5b load test: an empty dll under an old dll's name
}
ZIG_VERSION = "0.14.0"                                       # spec 1.3; checked once per run (review F9)
DLL_OBJS = ["core", "core_patch", "core_crash", "core_dllmain", "link", "chase", "stance", "pen"]


class BuildError(Exception):
    pass


def exe_args():
    b = os.environ.get("GOH_BIN", "").strip() or DEFAULT_BIN
    b = b.replace("\\", "/").rstrip("/")
    return [f"{b}/{n}" for n in EXE_NAMES]


def zig_env(target):
    env = os.environ.copy()
    env["ZIG_LOCAL_CACHE_DIR"] = str(OUT / "zigcache" / target)
    env["ZIG_GLOBAL_CACHE_DIR"] = str(OUT / "zigcache" / "global")
    return env


_ZIG_ID = None


def zig_id():
    """'zig 0.14.0, zig.exe sha256 <hex>' once per run; BuildError unless `zig version` prints ZIG_VERSION (a different compiler in the
    shared launcher folder would silently give another dll)"""
    global _ZIG_ID
    if _ZIG_ID is None:
        if not ZIG.exists():
            raise BuildError(f"zig not found at {ZIG}")
        p = subprocess.run([str(ZIG), "version"], capture_output=True, text=True)
        v = (p.stdout or "").strip()
        if p.returncode != 0 or v != ZIG_VERSION:
            raise BuildError(f"{ZIG} reports version '{v}' (exit {p.returncode}); the build needs zig {ZIG_VERSION}")
        _ZIG_ID = f"zig {v}, zig.exe sha256 {hashlib.sha256(ZIG.read_bytes()).hexdigest()}"
    return _ZIG_ID


class Target:
    def __init__(self, name):
        self.name = name
        self.obj = OUT / "obj" / name
        self.obj.mkdir(parents=True, exist_ok=True)
        # zig cc keeps the result of every compile in the LOCAL cache and replays a cache hit silently: an unchanged file that
        # warned once would pass the warning gate on the next run. Each target run starts with an empty local cache (the
        # global cache, which holds the expensive mingw CRT builds, is kept).
        shutil.rmtree(OUT / "zigcache" / name, ignore_errors=True)
        (OUT / "zigcache" / name).mkdir(parents=True, exist_ok=True)
        (OUT / "zigcache" / "global").mkdir(parents=True, exist_ok=True)
        self.cmds = self.obj / "commands.txt"
        self.runs = self.obj / "runs.txt"
        self.cmds.write_text("", encoding="utf-8")
        self.runs.write_text("", encoding="utf-8")

    def _record(self, f, cmd):
        with open(f, "a", encoding="utf-8") as fh:
            fh.write(subprocess.list2cmdline([str(c) for c in cmd]) + "\n")

    def _zig(self, cmd, what):
        zid = zig_id()
        if not self.cmds.read_text(encoding="utf-8"):
            with open(self.cmds, "a", encoding="utf-8") as fh:
                fh.write(f"# {zid}\n")                          # tools\parity.py P-7 skips '#' lines
        self._record(self.cmds, cmd)
        p = subprocess.run([str(c) for c in cmd], cwd=GM, env=zig_env(self.name), capture_output=True, text=True,
                           encoding="utf-8", errors="replace")
        text = (p.stdout or "") + (p.stderr or "")
        if text.strip():
            print(text.rstrip())
        if p.returncode != 0:
            raise BuildError(f"{what} failed (exit {p.returncode})")
        warns = [l for l in text.splitlines() if "warning:" in l]
        if warns:
            raise BuildError(f"{what}: {len(warns)} warning line(s) (the build allows none)")

    def need(self, rel, why=""):
        p = GM / rel
        if not p.exists():
            raise BuildError(f"{rel} does not exist yet{(' (' + why + ')') if why else ''}")
        return p

    def need_objects(self, objnames, files=()):
        """fail before compiling anything when a source (or another input) is missing; names every missing file"""
        miss = []
        for o in objnames:
            src, _, owner = OBJECTS[o]
            if not (GM / src).exists() and f"{src} ({owner} writes it)" not in miss:
                miss.append(f"{src} ({owner} writes it)")
        for rel, why in files:
            if not (GM / rel).exists():
                miss.append(f"{rel} ({why})")
        if miss:
            raise BuildError("missing input(s), not built: " + "; ".join(miss))

    def compile(self, objname, extra=()):
        src, flags, owner = OBJECTS[objname]
        self.need(src, f"{owner} writes it")
        suffix = "_sa" if extra else ""
        out = self.obj / f"{objname}{suffix}.o"
        cmd = [ZIG, "cc", "-c", *flags, *extra, src.replace("/", "\\"), "-o", out]
        print(f"[{self.name}] cc {src} {' '.join(extra)}".rstrip())
        self._zig(cmd, f"compile {src}")
        return out

    def link(self, objs, out, dll=False):
        out.parent.mkdir(parents=True, exist_ok=True)
        cmd = [ZIG, "cc", *LINK, *(["-shared", "-s"] if dll else []), *objs, "-lkernel32", "-o", out]
        print(f"[{self.name}] link {out.relative_to(GM)}")
        self._zig(cmd, f"link {out.name}")
        return out

    def run(self, exe, args, timeout=RUN_TIMEOUT):
        cmd = [str(exe), *[str(a) for a in args]]
        self._record(self.runs, cmd)
        print(f"[{self.name}] run {Path(exe).name} {' '.join(Path(a).name if '/' in str(a) else str(a) for a in args)}")
        p = subprocess.run(cmd, cwd=GM, capture_output=True, timeout=timeout)
        if p.stderr:
            sys.stdout.write(p.stderr.decode("utf-8", "replace"))
        return p.returncode, p.stdout


def lf(b):
    return b.replace(b"\r\n", b"\n")


def compare(out_bytes, ref, label):
    r = lf(Path(ref).read_bytes())
    o = lf(out_bytes)
    if o == r:
        print(f"  {label}: identical to {Path(ref).relative_to(GM)} ({len(r.splitlines())} lines)")
        return True
    ol, rl = o.split(b"\n"), r.split(b"\n")
    for i in range(max(len(ol), len(rl))):
        a = ol[i] if i < len(ol) else b"<end of output>"
        b = rl[i] if i < len(rl) else b"<end of reference>"
        if a != b:
            print(f"  {label}: DIFFERS from {Path(ref).relative_to(GM)} at line {i + 1}")
            print(f"    got:  {a.decode('latin-1')[:200]}")
            print(f"    want: {b.decode('latin-1')[:200]}")
            break
    return False


def last_line(b):
    lines = [l for l in lf(b).decode("latin-1").split("\n") if l.strip()]
    return lines[-1].strip() if lines else ""


# ---------------------------------------------------------------------------------------------------------------- targets
def t_core_test():
    t = Target("core-test")
    t.need_objects(["core", "core_patch", "core_crash", "test_core"])
    objs = [t.compile("core"), t.compile("core_patch"), t.compile("core_crash"), t.compile("test_core", ["-DTEST_CORE_STANDALONE"])]
    exe = t.link(objs, OUT / "wp0" / "core_test.exe")
    rc, out = t.run(exe, exe_args())
    (OUT / "wp0" / "core_test.txt").write_bytes(out)
    sys.stdout.write(lf(out).decode("latin-1"))
    if rc != 0 or last_line(out) != "ALL OK":
        raise BuildError(f"core_test.exe: exit {rc}, last line '{last_line(out)}' (want ALL OK, exit 0)")


def module_test(target, wp, objs_spec, exe_name, txt_name, ref):
    t = Target(target)
    t.need_objects(objs_spec)
    objs = [t.compile(o) for o in objs_spec]
    exe = t.link(objs, OUT / wp / exe_name)
    rc, out = t.run(exe, exe_args())
    (OUT / wp / txt_name).write_bytes(out)
    print(f"  exit {rc}, {len(lf(out).splitlines())} lines -> out\\{wp}\\{txt_name}, last line '{last_line(out)}'")
    if not compare(out, GM / ref, txt_name):
        raise BuildError(f"out\\{wp}\\{txt_name} differs from {ref}")
    if rc != 0:
        raise BuildError(f"{exe_name} exit {rc}")


def t_test_stance():
    module_test("test-stance", "wp1", ["core", "core_patch", "stance_t", "main_stance"], "stance_test.exe", "stance_test.txt",
                "baseline/ms_test_0.7.42.txt")


def t_test_pen():
    module_test("test-pen", "wp3", ["core", "core_patch", "pen_t", "main_pen"], "pen_test.exe", "pen_test.txt",
                "baseline/pf_test_0.1.1.txt")


def link_only(target, wp, module, exe_name):
    t = Target(target)
    t.need_objects(["core", "core_patch", "core_crash", module, "main_link_stub"])
    objs = [t.compile(o) for o in ["core", "core_patch", "core_crash", module, "main_link_stub"]]
    t.link(objs, OUT / wp / exe_name)
    print(f"  linked out\\{wp}\\{exe_name}")


def t_link_stance():
    link_only("link-stance", "wp1", "stance", "stance_link.exe")


def t_link_chase():
    link_only("link-chase", "wp2", "chase", "chase_link.exe")


def t_link_pen():
    link_only("link-pen", "wp3", "pen", "pen_link.exe")


def t_test_chase():
    t = Target("test-chase")
    t.need_objects(["test_chase"])
    obj = t.compile("test_chase", ["-DTEST_CHASE_STANDALONE"])
    exe = t.link([obj], OUT / "wp2" / "chase_test.exe")
    bad = []
    for x in exe_args():
        name = x.rsplit("/", 1)[-1]
        rc, out = t.run(exe, [x])
        (OUT / "wp2" / f"chase_{name}.txt").write_bytes(out)
        ok = compare(out, GM / "probe" / "chase" / f"out_{name}.txt", f"chase_{name}.txt")
        print(f"  exit {rc}, last line '{last_line(out)}'")
        if not ok or rc != 0:                                  # the probe exits 0: a crash after the full output still fails
            bad.append(f"{name}{'' if ok else ' (output differs)'}{'' if rc == 0 else f' (exit {rc})'}")
    if bad:
        raise BuildError(f"chase finder test failed for {', '.join(bad)}")


def pe_exports(path):
    d = Path(path).read_bytes()
    pe = struct.unpack_from("<I", d, 0x3C)[0]
    if d[pe:pe + 4] != b"PE\0\0":
        raise BuildError(f"{path}: not a PE file")
    nsec, = struct.unpack_from("<H", d, pe + 6)
    optsz, = struct.unpack_from("<H", d, pe + 20)
    opt = pe + 24
    if struct.unpack_from("<H", d, opt)[0] != 0x20B:
        raise BuildError(f"{path}: not PE32+")
    erva, esz = struct.unpack_from("<II", d, opt + 112)
    secs = []
    for i in range(nsec):
        s = opt + optsz + 40 * i
        vsz, va, rsz, roff = struct.unpack_from("<IIII", d, s + 8)
        secs.append((va, max(vsz, rsz), roff))

    def off(rva):
        for va, sz, ro in secs:
            if va <= rva < va + sz:
                return rva - va + ro
        raise BuildError(f"{path}: rva 0x{rva:X} not in a section")

    if not erva:
        return set()
    e = off(erva)
    nnames, = struct.unpack_from("<I", d, e + 24)
    names_rva, = struct.unpack_from("<I", d, e + 32)
    out = set()
    for i in range(nnames):
        nr, = struct.unpack_from("<I", d, off(names_rva) + 4 * i)
        o = off(nr)
        out.add(d[o:d.index(b"\0", o)].decode("ascii"))
    return out


def t_dll():
    t = Target("dll")
    t.need_objects(DLL_OBJS, [("src/gem_melee.ini", "WP4 builds it with tools/make_ini.py"), ("src/README.md", "WP4 writes it")])
    ini, readme = GM / "src" / "gem_melee.ini", GM / "src" / "README.md"
    objs = [t.compile(o) for o in DLL_OBJS]
    dll = t.link(objs, OUT / "dll" / "gem_melee.dll", dll=True)
    shutil.copyfile(ini, OUT / "dll" / "gem_melee.ini")
    shutil.copyfile(readme, OUT / "dll" / "README.md")
    ex = pe_exports(dll)
    print(f"  exports: {sorted(ex)}")
    if ex != {"gem_quant_listen"}:
        raise BuildError(f"exports are {sorted(ex)}, want ['gem_quant_listen']")


def t_test():
    t = Target("test")
    a = ["core", "core_patch", "core_crash", "stance_t", "pen_t", "test_chase", "test_core", "test_main", "link", "test_linked"]
    t.need_objects(a + DLL_OBJS + ["test_itest", "dummy_old"])
    objs = [t.compile(o) for o in a]
    t.link(objs, OUT / "test" / "gem_melee_test.exe")
    iobjs = [t.compile(o) for o in DLL_OBJS + ["test_itest"]]
    t.link(iobjs, OUT / "test" / "gem_melee_itest.exe")
    t.link([t.compile("dummy_old")], OUT / "test" / "dummy_old.dll", dll=True)   # parity P-5b (review F2)


def t_run_test():
    t = Target("run-test")
    exe = OUT / "test" / "gem_melee_test.exe"
    if not exe.exists() or not (OUT / "test" / "gem_melee_itest.exe").exists():
        raise BuildError("out\\test\\gem_melee_test.exe / gem_melee_itest.exe missing: run `build.py test` first")
    rc, out = t.run(exe, [], timeout=3600)
    (OUT / "test" / "test_output.txt").write_bytes(out)
    for l in lf(out).decode("latin-1").split("\n"):
        if re.match(r"^== .+: exit \d+$", l):                  # the driver's step lines (pen's own "== <exe>" lines are not)
            print("  " + l)
    print("  " + last_line(out))
    if rc != 0 or last_line(out) != "ALL OK":
        raise BuildError(f"gem_melee_test.exe: exit {rc}, last line '{last_line(out)}' (out\\test\\test_output.txt)")


def t_parity():
    t = Target("parity")
    t.need("tools/parity.py", "WP4 writes it")
    cmd = [sys.executable, str(GM / "tools" / "parity.py")]
    t._record(t.runs, cmd)
    p = subprocess.run(cmd, cwd=GM, capture_output=True, timeout=3600)
    out = p.stdout + p.stderr
    sys.stdout.write(lf(out).decode("latin-1"))
    if p.returncode != 0 or last_line(p.stdout) != "PARITY OK":
        raise BuildError(f"parity.py: exit {p.returncode}, last line '{last_line(p.stdout)}' (want PARITY OK)")


def tool_check(target, script, args, want):
    """run a GM tool script (python) and require exit 0 and a last line that starts with `want`"""
    t = Target(target)
    cmd = [sys.executable, str(GM / "tools" / script), *args]
    t._record(t.runs, cmd)
    p = subprocess.run(cmd, cwd=GM, capture_output=True, timeout=600)
    out = p.stdout + p.stderr
    sys.stdout.write(lf(out).decode("latin-1"))
    if p.returncode != 0 or not last_line(p.stdout).startswith(want):
        raise BuildError(f"{script} {' '.join(args)}: exit {p.returncode}, last line '{last_line(p.stdout)}' (want {want})")


def t_ini_check():
    tool_check("ini-check", "make_ini.py", ["--check"], "INI OK (")


def t_port_check():
    tool_check("port-check", "port_check.py", ["all"], "PORT CHECK OK")


def t_all():
    # review F3 / F6: the ini check and the port check are part of `all` (a renamed ini key or a removed / changed port edit passed it)
    for f in (t_ini_check, t_port_check, t_dll, t_test, t_run_test, t_parity):
        f()


def t_clean():
    if OUT.exists():
        shutil.rmtree(OUT)
    print("  out\\ deleted")


TARGETS = {
    "core-test": t_core_test, "test-stance": t_test_stance, "link-stance": t_link_stance, "test-chase": t_test_chase,
    "link-chase": t_link_chase, "test-pen": t_test_pen, "link-pen": t_link_pen, "dll": t_dll, "test": t_test,
    "run-test": t_run_test, "parity": t_parity, "ini-check": t_ini_check, "port-check": t_port_check, "all": t_all, "clean": t_clean,
}


def main() -> int:
    names = sys.argv[1:]
    if not names or any(n not in TARGETS for n in names):
        bad = [n for n in names if n not in TARGETS]
        if bad:
            print(f"unknown target(s): {' '.join(bad)}")
        print(__doc__)
        return 2
    rc = 0
    for n in names:
        print(f"[build] {n}")
        try:
            TARGETS[n]()
            print(f"[build] {n}: OK")
        except BuildError as e:
            print(f"[build] {n}: FAILED ({e})")
            rc = 1
        except subprocess.TimeoutExpired as e:
            print(f"[build] {n}: FAILED (timeout: {e.cmd})")
            rc = 1
    return rc


if __name__ == "__main__":
    sys.exit(main())
