"""Windows counterpart of tests/run_offline_check.sh (no Linux / WSL needed), with zig cc:
  1. build dist\\gem_fauna.dll exactly as build.bat does (+ gem_fauna.ini), and once more with -Werror
  2. fx_test.exe (FX_TEST build; tests\\win\\sys\\mman.h maps each exe at its image base with VirtualAlloc, the test exe
     itself is linked at another base) run against every exe given -> every finder, incl. "head icon finders"
  3. tests\\headicon_gate_test.c against this source (expectations of 0.12.2 and later) and, if given, against an older source
     folder with -DEXPECT_0121
usage: python -B tests\\run_offline_check_win.py <zig.exe> <old source dir or -> <exe> [<exe> ...]
Output goes to stdout; the caller keeps it (out\\test_output.txt).
"""
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WARN = ["-Wall", "-Wextra", "-Wno-unused-parameter", "-Wno-missing-field-initializers"]


def run(args, cwd=HERE):
    r = subprocess.run(args, cwd=cwd, capture_output=True)
    text = (r.stdout + r.stderr).decode("utf-8", "replace")
    return r.returncode, text


def main():
    zig, old, exes = sys.argv[1], sys.argv[2], sys.argv[3:]
    ok = True
    os.makedirs(os.path.join(HERE, "dist"), exist_ok=True)
    os.makedirs(os.path.join(HERE, "out"), exist_ok=True)
    print("=== 1. dll build (build.bat flags)")
    rc, t = run([zig, "cc", "-target", "x86_64-windows-gnu", "-O2", "-s", "-shared"] + WARN +
                ["-o", r"dist\gem_fauna.dll", r"src\gem_fauna.c", "-lkernel32", "-luser32"])
    print("exit", rc, "| compiler output:", t.strip() or "none (no warnings)")
    ok &= rc == 0
    for f in os.listdir(os.path.join(HERE, "dist")):
        if f.endswith((".lib", ".pdb")):
            os.remove(os.path.join(HERE, "dist", f))
    ini = open(os.path.join(HERE, "gem_fauna.ini"), "rb").read()
    with open(os.path.join(HERE, "dist", "gem_fauna.ini.tmp"), "wb") as f:
        f.write(ini)
    os.replace(os.path.join(HERE, "dist", "gem_fauna.ini.tmp"), os.path.join(HERE, "dist", "gem_fauna.ini"))
    rc, t = run([zig, "cc", "-target", "x86_64-windows-gnu", "-O2", "-shared"] + WARN + ["-Werror", "-o", r"out\werror_check.dll", r"src\gem_fauna.c", "-lkernel32", "-luser32"])
    print("-Werror build: exit", rc, t.strip() or "(no warnings)")
    ok &= rc == 0

    print("=== 2. finder check (FX_TEST)")
    fx = os.path.join(HERE, "out", "fx_test.exe")
    rc, t = run([zig, "cc", "-target", "x86_64-windows-gnu", "-O1", "-DFX_TEST", "-I", r"tests\win", "-Wall", "-Wextra",
                 "-Wno-unused-parameter", "-Wno-unused-function", "-Wno-unused-variable",
                 "-Wl,--image-base=0x10000000", "-o", fx, r"src\gem_fauna.c", "-lkernel32"])
    print("fx_test build: exit", rc, t.strip() or "")
    ok &= rc == 0
    for exe in exes:
        rc, t = run([fx, exe])
        print("--- %s: exit %d" % (exe, rc))
        print(t.rstrip())
        ok &= rc == 0

    print("=== 3. head icon gate test")
    cases = [("this source", os.path.join(HERE, "src"), [])]
    if old != "-":
        cases.append(("old source " + old, os.path.join(old, "src"), ["-DEXPECT_0121"]))
    for i, (what, inc, extra) in enumerate(cases):
        exe = os.path.join(HERE, "out", "headicon_gate_test%d.exe" % i)
        src_name = os.path.join(inc, "fx", "headicon.c").replace("\\", "/")
        rc, t = run([zig, "cc", "-target", "x86_64-windows-gnu", "-O1", "-Wall", "-Wextra", "-Wno-unused-parameter",
                     "-Wno-unused-function", "-I", inc, '-DFX_HEADICON_SRC="%s"' % src_name] + extra +
                    ["-o", exe, r"tests\headicon_gate_test.c"])
        print("--- build against", what, ": exit", rc, t.strip() or "")
        ok &= rc == 0
        rc, t = run([exe])
        print(t.rstrip())
        ok &= rc == 0
    for f in os.listdir(os.path.join(HERE, "out")):
        if f.endswith((".pdb", ".lib")) or f == "werror_check.dll":
            os.remove(os.path.join(HERE, "out", f))
    print("RESULT:", "ALL OK" if ok else "FAILED")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
