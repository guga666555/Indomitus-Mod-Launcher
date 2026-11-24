"""Copy the three source DLL trees (and the bits of the ecosystem the merge depends on) into inputs/ as
read-only reference snapshots, with a SHA-256 manifest.

The originals belong to other sessions / worktrees and may keep changing; the merge is made against these
snapshots, and `--check` reports which original has moved since (so its diff can be ported).

usage: python snapshot_inputs.py            first snapshot (refuses to overwrite an existing inputs/ tree)
       python snapshot_inputs.py --check    compare the originals with the manifest
"""
import hashlib
import json
import os
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
INPUTS = os.path.join(ROOT, "inputs")
MANIFEST = os.path.join(INPUTS, "MANIFEST.json")

MC054 = "C:/Users/alex_/Documents/IndomitusLauncher_mc054/_dlls"
PROJ = "C:/Users/alex_/OneDrive/Desktop/Claude_Indomitus_Project"
PAYLOAD = "C:/Users/alex_/AppData/Local/Indomitus Launcher/payload/a1d6ed3c"

# name -> (source dir, what it is)
TREES = {
    "meleestance_0.7.42": (MC054 + "/_indomitus_DLL_meleestance",
                           "GEM MeleeStance 0.7.42 (mc054 worktree, built Nov 20 2026 12:40, not in any payload yet)"),
    "meleechase_0.5.4": (MC054 + "/_indomitus_DLL_meleechase",
                         "GEM MeleeChase 0.5.4 (mc054 worktree; dist dll = live payload a1d6ed3c)"),
    "penfilter_0.1.1": (PROJ + "/_tools/gem_penfilter",
                        "GEM PenFilter 0.1.1 (out dll = live payload a1d6ed3c)"),
    "teleport_0.2.13": (MC054 + "/_indomitus_DLL_teleport",
                        "GEM Teleport 0.2.13 (looks up gem_meleestance.dll!gem_quant_listen; exports gem_order_listen)"),
}
# single files worth keeping next to the sources (live logs = ground truth for the editor addresses)
FILES = {
    "live_payload_a1d6ed3c": [PAYLOAD + "/" + n for n in (
        "gem_meleestance.dll", "gem_meleestance.ini", "gem_meleestance_editor.log",
        "gem_meleechase.dll", "gem_meleechase.ini", "gem_meleechase.log",
        "gem_penfilter.dll", "gem_penfilter.ini", "gem_penfilter_editor.log",
        "gem_teleport.dll", "gem_teleport.ini", "gem_teleport.log")],
}
SKIP_DIRS = {".zig-cache", "zig", "__pycache__"}
SKIP_EXT = {".obj", ".pdb", ".lib", ".exp", ".zip"}
SKIP_PREFIX = ("gem_meleestance.c.bak", "gem_meleestance.c.pre", "gem_meleestance.ini.bak")


def sha(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for blk in iter(lambda: f.read(1 << 20), b""):
            h.update(blk)
    return h.hexdigest()


def walk(src):
    for d, dirs, files in os.walk(src):
        dirs[:] = [x for x in dirs if x not in SKIP_DIRS]
        for n in files:
            if os.path.splitext(n)[1].lower() in SKIP_EXT or n.startswith(SKIP_PREFIX):
                continue
            p = os.path.join(d, n)
            yield p, os.path.relpath(p, src).replace("\\", "/")


def snapshot():
    if os.path.exists(MANIFEST):
        sys.exit("inputs/ already holds a snapshot (MANIFEST.json). Use --check, or move the old one away first.")
    man = {"trees": {}, "files": {}}
    for name, (src, what) in TREES.items():
        ent = {"source": src, "what": what, "files": {}}
        for p, rel in walk(src):
            dst = os.path.join(INPUTS, name, rel)
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            shutil.copy2(p, dst)
            ent["files"][rel] = sha(dst)
        man["trees"][name] = ent
        print(f"{name}: {len(ent['files'])} file(s) from {src}")
    for name, paths in FILES.items():
        ent = {}
        for p in paths:
            dst = os.path.join(INPUTS, name, os.path.basename(p))
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            shutil.copy2(p, dst)
            ent[p] = sha(dst)
        man["files"][name] = ent
        print(f"{name}: {len(ent)} file(s)")
    with open(MANIFEST, "w", encoding="utf-8", newline="\n") as f:
        json.dump(man, f, indent=1)
    print("wrote", MANIFEST)


def check():
    man = json.load(open(MANIFEST, encoding="utf-8"))
    moved = 0
    for name, ent in man["trees"].items():
        now = {rel: sha(p) for p, rel in walk(ent["source"])}
        for rel, h in ent["files"].items():
            if rel not in now:
                print(f"GONE     {name}/{rel}"); moved += 1
            elif now[rel] != h:
                print(f"CHANGED  {name}/{rel}"); moved += 1
        for rel in now:
            if rel not in ent["files"]:
                print(f"NEW      {name}/{rel}"); moved += 1
    print("originals unchanged since the snapshot" if not moved else f"{moved} difference(s): port them into the merge")
    return moved


def main():
    if "--check" in sys.argv:
        sys.exit(1 if check() else 0)
    snapshot()


if __name__ == "__main__":
    main()
