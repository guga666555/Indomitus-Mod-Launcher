"""Check GEM Teleport 0.2.14 (gem_melee spec 7.3). Prints the findings and "OK", else "FAILED" (exit 1).

usage: python compat\\teleport_0.2.14\\check.py

1. 0.2.13 rebuilt from compat\\teleport_rebuild (copied byte for byte into _work\\, so nothing is written there) with build.bat's
   flags has SHA-256 98D4F8FE...052D (= the live payload a1d6ed3c dll);
2. this folder = compat\\teleport_rebuild (minus dist) + exactly the spec 7.3 edits: src\\gem_teleport.c:53,
   src\\tp\\teleport.c:498-510 (the new tp_quant_register, text compared with the spec), one changelog line at the top of
   README.md "## Changes", gem_teleport.ini:58; nothing else differs, CRLF / ASCII kept; extra files only this tool's own;
3. teleport_0.2.14.patch = the unified diff of the two trees (and, when git is on PATH, `git apply` of it onto a copy of 0.2.13
   gives the 0.2.14 files);
4. a fresh build of this folder (same flags, no warnings) is byte-identical to dist\\gem_teleport.dll, dist\\gem_teleport.ini =
   gem_teleport.ini, and the dll exports exactly {gem_order_listen} and names gem_melee.dll and gem_meleestance.dll.
"""
import difflib
import hashlib
import os
import shutil
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
COMPAT = os.path.dirname(HERE)
GM = os.path.dirname(COMPAT)
REBUILD = os.path.join(COMPAT, "teleport_rebuild")
LIVE_DLL = os.path.join(GM, "inputs", "live_payload_a1d6ed3c", "gem_teleport.dll")
ZIG = r"C:\Users\alex_\Documents\IndomitusLauncher\zig\zig.exe"
WORK = os.path.join(HERE, "_work")
SHA_0213 = "98D4F8FEB41403F21B93BFE34C4EB471B6B15A2C433DDE656F1BDBD488B3052D"
BUILD_FLAGS = ["cc", "-target", "x86_64-windows-gnu", "-O2", "-s", "-shared", "-Wall", "-Wextra", "-Wno-unused-parameter",
               "-Wno-missing-field-initializers"]
OWN_FILES = {"check.py", "make_0.2.14.py", "teleport_0.2.14.patch", os.path.join("dist", "gem_teleport.dll"),
             os.path.join("dist", "gem_teleport.ini")}

# spec 7.3, the new tp_quant_register (teleport.c:498-510 of 0.2.13 become these lines)
SPEC_REG = '''static void tp_quant_register(void)
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

# allowed changes, in 0.2.13 line numbers (1-based): (first deleted line, last deleted line, max added lines, check of the new lines)
def _is_version(lines):
    return lines == ['#define FX_VERSION "0.2.14"']

def _is_reg(lines):
    return lines == SPEC_REG.rstrip("\n").split("\n")

def _is_readme(lines):
    # line 20 of 0.2.13 is the blank line under "## Changes"; it becomes blank, the 0.2.14 entry, blank (difflib may place the
    # inserted pair on either side of that blank line, so the rule covers it)
    return (len(lines) == 3 and lines[0] == "" and lines[1].startswith("**0.2.14** (Nov 20 2026") and "gem_melee.dll" in lines[1]
            and "gem_meleestance.dll" in lines[1] and lines[2] == "")

def _is_ini(lines):
    return (len(lines) == 1 and lines[0].startswith("; entity script-tag vector offset (1.067: 0x4E8")
            and "gem_melee.dll" in lines[0] and "gem_meleestance.dll" in lines[0] and lines[0].endswith("only change after an engine update"))

ALLOWED = {
    os.path.join("src", "gem_teleport.c"): [(53, 53, 1, _is_version)],
    os.path.join("src", "tp", "teleport.c"): [(498, 510, 20, _is_reg)],
    "README.md": [(20, 20, 3, _is_readme)],                    # the blank line 20 under "## Changes" + the new top entry
    "gem_teleport.ini": [(58, 58, 1, _is_ini)],
}


def tree_files(root, skip_top):
    out = []
    for d, dirs, files in os.walk(root):
        rel = os.path.relpath(d, root)
        if rel == ".":
            dirs[:] = [x for x in dirs if x not in skip_top]
        for f in files:
            out.append(f if rel == "." else os.path.normpath(os.path.join(rel, f)))
    return sorted(out)


def sha(p):
    with open(p, "rb") as f:
        return hashlib.sha256(f.read()).hexdigest().upper()


def read(p):
    with open(p, "rb") as f:
        return f.read()


def exports(path):
    data = read(path)
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    nsec = struct.unpack_from("<H", data, pe + 6)[0]
    optsz = struct.unpack_from("<H", data, pe + 20)[0]
    opt = pe + 24
    so = opt + optsz
    secs = [struct.unpack_from("<IIII", data, so + i * 40 + 8) for i in range(nsec)]

    def off(rva):
        for vsz, va, rsz, rptr in secs:
            if va <= rva < va + max(vsz, rsz):
                return rptr + rva - va
        raise ValueError(f"rva {rva:#x} in no section")

    def cstr(rva):
        o = off(rva)
        return data[o:data.index(b"\0", o)].decode("latin1")

    exp_rva = struct.unpack_from("<I", data, opt + 112)[0]
    names = set()
    if exp_rva:
        o = off(exp_rva)
        n = struct.unpack_from("<I", data, o + 24)[0]
        names_rva = struct.unpack_from("<I", data, o + 32)[0]
        for i in range(n):
            names.add(cstr(struct.unpack_from("<I", data, off(names_rva) + 4 * i)[0]))
    return names


def zig_build(src_root, out_dll):
    os.makedirs(os.path.dirname(out_dll), exist_ok=True)
    if os.path.exists(out_dll):
        os.remove(out_dll)
    env = dict(os.environ)
    env["ZIG_LOCAL_CACHE_DIR"] = os.path.join(WORK, "zigcache", "local")
    env["ZIG_GLOBAL_CACHE_DIR"] = os.path.join(WORK, "zigcache", "global")
    cmd = [ZIG] + BUILD_FLAGS + ["-o", out_dll, os.path.join("src", "gem_teleport.c"), "-lkernel32", "-luser32", "-lm"]
    p = subprocess.run(cmd, cwd=src_root, env=env, capture_output=True, text=True)
    for f in os.listdir(os.path.dirname(out_dll)):
        if f.lower().endswith(".lib"):
            os.remove(os.path.join(os.path.dirname(out_dll), f))
    return p.returncode, (p.stdout or "") + (p.stderr or "")


def copy_tree(src, dst, files):
    if os.path.isdir(dst):
        shutil.rmtree(dst)
    for rel in files:
        os.makedirs(os.path.dirname(os.path.join(dst, rel)), exist_ok=True)
        shutil.copyfile(os.path.join(src, rel), os.path.join(dst, rel))


def unified_patch(a_root, b_root, files):
    parts = []
    for rel in files:
        a = read(os.path.join(a_root, rel)).decode("ascii").splitlines(keepends=True)
        b = read(os.path.join(b_root, rel)).decode("ascii").splitlines(keepends=True)
        if a != b:
            rp = rel.replace(os.sep, "/")
            parts.extend(difflib.unified_diff(a, b, fromfile="a/" + rp, tofile="b/" + rp, n=3))
    return "".join(parts)


def main():
    bad = []
    info = []
    os.makedirs(WORK, exist_ok=True)
    base_files = tree_files(REBUILD, {"dist"})

    # 1. 0.2.13 rebuilt from an exact copy of compat\teleport_rebuild
    src13 = os.path.join(WORK, "src_0.2.13")
    copy_tree(REBUILD, src13, base_files)
    for rel in base_files:
        if read(os.path.join(src13, rel)) != read(os.path.join(REBUILD, rel)):
            bad.append(f"1: copy of {rel} differs from compat\\teleport_rebuild")
    dll13 = os.path.join(WORK, "rebuild_0.2.13", "gem_teleport.dll")
    rc, out = zig_build(src13, dll13)
    if rc != 0 or not os.path.isfile(dll13):
        bad.append(f"1: 0.2.13 rebuild failed (exit {rc}): {out.strip()[:400]}")
    else:
        h = sha(dll13)
        info.append(f"0.2.13 rebuilt from compat\\teleport_rebuild: SHA-256 {h}")
        if h != SHA_0213:
            bad.append(f"1: 0.2.13 rebuild SHA-256 {h} != {SHA_0213}")
        if os.path.isfile(LIVE_DLL) and sha(LIVE_DLL) != h:
            bad.append("1: 0.2.13 rebuild differs from inputs\\live_payload_a1d6ed3c\\gem_teleport.dll")
        if "warning:" in out:
            bad.append("1: 0.2.13 rebuild printed warnings")

    # 2. the tree diff touches only the spec 7.3 lines
    here_files = tree_files(HERE, {"_work"})
    for rel in base_files:
        if rel not in here_files:
            bad.append(f"2: {rel} of 0.2.13 is missing here")
    for rel in here_files:
        if rel not in base_files and rel not in OWN_FILES:
            bad.append(f"2: extra file {rel}")
    changed = []
    for rel in base_files:
        if rel not in here_files:
            continue
        a, b = read(os.path.join(REBUILD, rel)), read(os.path.join(HERE, rel))
        if a == b:
            continue
        changed.append(rel)
        if rel not in ALLOWED:
            bad.append(f"2: {rel} changed, but spec 7.3 lists no edit for it")
            continue
        try:
            bt = b.decode("ascii")
        except UnicodeDecodeError:
            bad.append(f"2: {rel} is not ASCII any more")
            continue
        if bt.count("\n") != bt.count("\r\n") or not bt.endswith("\r\n"):
            bad.append(f"2: {rel} is not CRLF-only any more")
        al = a.decode("ascii").replace("\r\n", "\n").split("\n")
        bl = bt.replace("\r\n", "\n").split("\n")
        rules = ALLOWED[rel]
        used = [0] * len(rules)
        for tag, i1, i2, j1, j2 in difflib.SequenceMatcher(None, al, bl, autojunk=False).get_opcodes():
            if tag == "equal":
                continue
            ok = False
            for k, (f, t, add_max, test) in enumerate(rules):
                inside = (f <= i1 + 1 and i2 <= t) if i2 > i1 else (f <= i1 + 1 <= t + 1)
                if inside:
                    used[k] += j2 - j1
                    if used[k] > add_max:
                        bad.append(f"2: {rel}: rule {f}-{t} adds {used[k]} lines (max {add_max})")
                    ok = True
                    break
            if not ok:
                bad.append(f"2: {rel}: change at 0.2.13 lines {i1 + 1}-{i2} ({tag}) is outside the spec 7.3 lines")
        # the new text of each rule = the 0.2.14 lines between the unchanged neighbours
        for f, t, add_max, test in rules:
            pre = al[:f - 1]
            post = al[t:]
            if bl[:len(pre)] != pre or (post and bl[len(bl) - len(post):] != post):
                bad.append(f"2: {rel}: lines outside {f}-{t} changed")
                continue
            new = bl[len(pre):len(bl) - len(post)]
            if not test(new):
                bad.append(f"2: {rel}: the new lines at {f}-{t} are not the spec 7.3 text: {new!r}"[:600])
            else:
                info.append(f"{rel}: 0.2.13 lines {f}-{t} ({t - f + 1} lines) -> {len(new)} lines, as in spec 7.3")
    for rel in ALLOWED:
        if rel not in changed:
            bad.append(f"2: {rel}: the spec 7.3 edit is missing")

    # 3. the .patch
    patch_path = os.path.join(HERE, "teleport_0.2.14.patch")
    if not os.path.isfile(patch_path):
        bad.append("3: teleport_0.2.14.patch missing")
    else:
        want = unified_patch(REBUILD, HERE, base_files)
        if read(patch_path) != want.encode("ascii"):
            bad.append("3: teleport_0.2.14.patch is not the diff of the two trees (rerun make_0.2.14.py)")
        git = shutil.which("git")
        if git:
            at = os.path.join(WORK, "apply_test")
            copy_tree(REBUILD, at, base_files)
            p = subprocess.run([git, "apply", "--whitespace=nowarn", patch_path], cwd=at, capture_output=True, text=True)
            if p.returncode != 0:
                bad.append(f"3: git apply failed: {(p.stdout + p.stderr).strip()[:400]}")
            else:
                diff = [rel for rel in base_files if read(os.path.join(at, rel)) != read(os.path.join(HERE, rel))]
                if diff:
                    bad.append(f"3: git apply of the patch onto 0.2.13 does not give the 0.2.14 files: {diff}")
                else:
                    info.append(f"git apply of teleport_0.2.14.patch onto a copy of 0.2.13 gives the 0.2.14 files ({len(changed)} changed)")
        else:
            info.append("git not on PATH: patch apply test skipped (the patch still equals the tree diff)")

    # 4. dist = a fresh build; exports
    dist = os.path.join(HERE, "dist", "gem_teleport.dll")
    fresh = os.path.join(WORK, "build_0.2.14", "gem_teleport.dll")
    src14 = os.path.join(WORK, "src_0.2.14")
    copy_tree(HERE, src14, base_files)
    rc, out = zig_build(src14, fresh)
    if rc != 0 or not os.path.isfile(fresh):
        bad.append(f"4: 0.2.14 build failed (exit {rc}): {out.strip()[:400]}")
    elif "warning:" in out:
        bad.append(f"4: 0.2.14 build printed warnings: {out.strip()[:400]}")
    if not os.path.isfile(dist):
        bad.append("4: dist\\gem_teleport.dll missing (run make_0.2.14.py)")
    elif os.path.isfile(fresh):
        if read(dist) != read(fresh):
            bad.append("4: dist\\gem_teleport.dll is not what the 0.2.14 sources build (rerun make_0.2.14.py)")
        ex = exports(dist)
        info.append(f"0.2.14 dist\\gem_teleport.dll: SHA-256 {sha(dist)}, {os.path.getsize(dist)} bytes, exports {sorted(ex)}")
        if ex != {"gem_order_listen"}:
            bad.append(f"4: exports of 0.2.14 = {sorted(ex)}, expected ['gem_order_listen']")
        d = read(dist)
        for s in (b"gem_melee.dll", b"gem_meleestance.dll", b"0.2.14", b"aim: per-quant tick from %s (gem_quant_listen) registered"):
            if s not in d:
                bad.append(f"4: {s!r} not in the 0.2.14 dll")
        if b'"0.2.13"' in d or b"GEM MeleeStance (gem_quant_listen)" in d:
            bad.append("4: the 0.2.14 dll still carries 0.2.13 text")
    if os.path.isfile(dll13):
        info.append(f"0.2.13 rebuild exports {sorted(exports(dll13))}")
    di = os.path.join(HERE, "dist", "gem_teleport.ini")
    if not os.path.isfile(di) or read(di) != read(os.path.join(HERE, "gem_teleport.ini")):
        bad.append("4: dist\\gem_teleport.ini is not gem_teleport.ini (build.bat copies it)")

    for line in info:
        print(line)
    if bad:
        for line in bad:
            print("FAIL " + line)
        print("FAILED")
        return 1
    print("OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
