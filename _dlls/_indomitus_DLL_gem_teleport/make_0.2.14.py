"""Make GEM Teleport 0.2.14 (gem_melee spec 7.3): 0.2.13 + a two-name lookup for the per-quant tick.

usage: python compat\\teleport_0.2.14\\make_0.2.14.py

1. copies compat\\teleport_rebuild\\ (= inputs\\teleport_0.2.13, no dist) into this folder (only read there);
2. applies exactly the spec 7.3 edits (each old text must match exactly once; the files stay CRLF):
     src\\gem_teleport.c:53         FX_VERSION "0.2.14"
     src\\tp\\teleport.c:498-510    tp_quant_register: gem_melee.dll first, then gem_meleestance.dll
     README.md                     one changelog line at the top of "## Changes"
     gem_teleport.ini:58           the comment names gem_melee.dll too
3. writes teleport_0.2.14.patch (unified diff vs 0.2.13, a/ b/ paths, CRLF content lines as in the files);
4. builds dist\\gem_teleport.dll with Teleport's own build.bat flags (L21-22) and copies gem_teleport.ini to dist\\ (build.bat
   L27); no payload copy. zig = C:\\Users\\alex_\\Documents\\IndomitusLauncher\\zig\\zig.exe (spec 1.3), caches in _work\\zigcache\\.
Then run check.py.
"""
import difflib
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
COMPAT = os.path.dirname(HERE)
GM = os.path.dirname(COMPAT)
REBUILD = os.path.join(COMPAT, "teleport_rebuild")
ZIG = r"C:\Users\alex_\Documents\IndomitusLauncher\zig\zig.exe"
WORK = os.path.join(HERE, "_work")

# the files copied from teleport_rebuild (everything there except dist\)
def tree_files(root):
    out = []
    for d, dirs, files in os.walk(root):
        rel = os.path.relpath(d, root)
        if rel == "." :
            dirs[:] = [x for x in dirs if x != "dist"]
        for f in files:
            out.append(os.path.normpath(os.path.join(rel, f)) if rel != "." else f)
    return sorted(out)

NEW_REG = r'''static void tp_quant_register(void)
{
    if (g_quantReg) return;
    ULONGLONG now = GetTickCount64();
    if (now - g_quantTry < 1000) return;
    g_quantTry = now;
    static const char *const names[2] = { "gem_melee.dll", "gem_meleestance.dll" };   /* 0.2.14: GEM Melee replaces GEM MeleeStance */
    void (*reg)(void (*)(void)) = NULL; const char *who = NULL; int loaded = 0;
    for (int i = 0; i < 2 && !reg; i++) {
        HMODULE h = GetModuleHandleA(names[i]);
        if (!h) continue;
        loaded = 1;
        reg = (void (*)(void (*)(void)))(void *)GetProcAddress(h, "gem_quant_listen");
        if (reg) who = names[i];
    }
    if (!reg) { if (g_aimLogged < 2) { logf_("aim: gem_melee.dll / gem_meleestance.dll %s - no per-quant tick, teleport_aim_end may lag until the next order", loaded ? "has no gem_quant_listen export" : "not loaded"); g_aimLogged++; } return; }
    reg(tp_quant);
    g_quantReg = 1;
    logf_("aim: per-quant tick from %s (gem_quant_listen) registered", who);
}
'''

OLD_REG = r'''static void tp_quant_register(void)
{
    if (g_quantReg) return;
    ULONGLONG now = GetTickCount64();
    if (now - g_quantTry < 1000) return;
    g_quantTry = now;
    HMODULE h = GetModuleHandleA("gem_meleestance.dll");
    void (*reg)(void (*)(void)) = h ? (void (*)(void (*)(void)))(void *)GetProcAddress(h, "gem_quant_listen") : NULL;
    if (!reg) { if (g_aimLogged < 2) { logf_("aim: gem_meleestance.dll %s - no per-quant tick, teleport_aim_end may lag until the next order", h ? "has no gem_quant_listen export" : "not loaded"); g_aimLogged++; } return; }
    reg(tp_quant);
    g_quantReg = 1;
    logf_("aim: per-quant tick from GEM MeleeStance (gem_quant_listen) registered");
}
'''

README_LINE = ("**0.2.14** (Nov 20 2026, GEM Melee): the per-quant tick (`gem_quant_listen`) is looked up in `gem_melee.dll` first, "
               "then `gem_meleestance.dll`, and the log line names the dll it registered with; nothing else changed.")

def crlf(s):
    return s.replace("\r\n", "\n").replace("\n", "\r\n")

# (file, old text, new text): each old text must occur exactly once in the 0.2.13 file
EDITS = [
    ("src/gem_teleport.c", '#define FX_VERSION "0.2.13"\r\n', '#define FX_VERSION "0.2.14"\r\n'),
    ("src/tp/teleport.c", crlf(OLD_REG), crlf(NEW_REG)),
    ("README.md", "## Changes\r\n\r\n**0.1.2**", "## Changes\r\n\r\n" + README_LINE + "\r\n\r\n**0.1.2**"),
    ("gem_teleport.ini",
     "; entity script-tag vector offset (1.067: 0x4E8, the one GEM MeleeStance uses); only change after an engine update\r\n",
     "; entity script-tag vector offset (1.067: 0x4E8, the one GEM MeleeStance uses: gem_meleestance.dll, now gem_melee.dll); "
     "only change after an engine update\r\n"),
]

BUILD_FLAGS = ["cc", "-target", "x86_64-windows-gnu", "-O2", "-s", "-shared", "-Wall", "-Wextra", "-Wno-unused-parameter",
               "-Wno-missing-field-initializers"]


def zig_env():
    env = dict(os.environ)
    env["ZIG_LOCAL_CACHE_DIR"] = os.path.join(WORK, "zigcache", "local")
    env["ZIG_GLOBAL_CACHE_DIR"] = os.path.join(WORK, "zigcache", "global")
    return env


def zig_build(src_root, out_dll):
    """build.bat L21-22 in src_root (relative source path, as build.bat runs it); returns (exit code, output text)."""
    os.makedirs(os.path.dirname(out_dll), exist_ok=True)
    cmd = [ZIG] + BUILD_FLAGS + ["-o", out_dll, os.path.join("src", "gem_teleport.c"), "-lkernel32", "-luser32", "-lm"]
    p = subprocess.run(cmd, cwd=src_root, env=zig_env(), capture_output=True, text=True)
    out = (p.stdout or "") + (p.stderr or "")
    for f in os.listdir(os.path.dirname(out_dll)):                     # build.bat L26: del dist\*.lib
        if f.lower().endswith(".lib"):
            os.remove(os.path.join(os.path.dirname(out_dll), f))
    return p.returncode, out, cmd


def unified_patch(a_root, b_root, files):
    parts = []
    for rel in files:
        with open(os.path.join(a_root, rel), "rb") as f:
            a = f.read().decode("ascii").splitlines(keepends=True)
        with open(os.path.join(b_root, rel), "rb") as f:
            b = f.read().decode("ascii").splitlines(keepends=True)
        if a == b:
            continue
        rp = rel.replace(os.sep, "/")
        parts.extend(difflib.unified_diff(a, b, fromfile="a/" + rp, tofile="b/" + rp, n=3))
    return "".join(parts)


def main():
    os.makedirs(WORK, exist_ok=True)
    files = tree_files(REBUILD)
    for rel in files:
        dst = os.path.join(HERE, rel)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        shutil.copyfile(os.path.join(REBUILD, rel), dst)
    for rel, old, new in EDITS:
        p = os.path.join(HERE, rel)
        with open(p, "rb") as f:
            t = f.read().decode("ascii")
        n = t.count(old)
        if n != 1:
            print(f"EDIT FAILED: {rel}: the 0.2.13 text occurs {n} times (expected 1)")
            return 1
        with open(p, "wb") as f:
            f.write(t.replace(old, new).encode("ascii"))
    patch = unified_patch(REBUILD, HERE, files)
    with open(os.path.join(HERE, "teleport_0.2.14.patch"), "wb") as f:
        f.write(patch.encode("ascii"))
    dist = os.path.join(HERE, "dist")
    rc, out, cmd = zig_build(HERE, os.path.join(dist, "gem_teleport.dll"))
    if out.strip():
        print(out.strip())
    if rc != 0 or "warning:" in out:
        print(f"BUILD FAILED (exit {rc})")
        return 1
    shutil.copyfile(os.path.join(HERE, "gem_teleport.ini"), os.path.join(dist, "gem_teleport.ini"))   # build.bat L27
    with open(os.path.join(WORK, "build_command.txt"), "w", encoding="ascii") as f:
        f.write("cwd: " + HERE + "\n" + " ".join(cmd) + "\n")
    print(f"copied {len(files)} files, applied {len(EDITS)} edits, wrote teleport_0.2.14.patch, built dist\\gem_teleport.dll")
    return 0


if __name__ == "__main__":
    sys.exit(main())
