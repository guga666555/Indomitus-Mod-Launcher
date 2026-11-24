"""Assault marine breeds and the "may melee outside the stance" breed tag ([MeleeStance] AutoMeleeTag of MeleeStance 0.7.42 /
GEM Melee).

Nov 21 2026 history (Alex): "change assault marines" -> tagged melee_infantry; "remove the melee tag ... I don't want just the one
zerging forwards" (melee_infantry is also [Zerg]'s tag) -> removed; "do both" -> tagged melee_auto (AutoMeleeTag=
melee_infantry,melee_auto: may melee, no [Zerg] charge) and the thunder-hammer veteran got his pistol in hand
(tools/hammer_vet_pistol_in_hand.py).

usage: python tag_assault_marines.py [--tag <word>] [--remove] [--apply]      default tag melee_auto, dry run without --apply
Each file is re-read right before its edit, only the {tags ...} line changes (the tag is appended to / taken out of the existing
list), CRLF kept. The first --apply of a tag keeps a backup in Documents/Indomitus_backups/_tools/gem_melee/bak_assault_tags/.
"""
import os
import re
import shutil
import sys

MOD = "C:/Program Files (x86)/Steam/steamapps/common/Call to Arms - Gates of Hell/mods/Indomitus_DEV"
DIR = MOD + "/resource/set/breed/mp/sm/warhammer"
BAK = "C:/Users/alex_/Documents/Indomitus_backups/_tools/gem_melee/bak_assault_tags"
TAG = ('"' + (sys.argv[sys.argv.index("--tag") + 1] if "--tag" in sys.argv else "melee_auto") + '"').encode()
FILES = [
    "um_assault.set", "um_assault_flamer.set", "um_assault_plasma.set", "um_assault_sgt.set", "um_assault_sgt_fist.set",
    "um_vet_assault.set", "um_vet_assault_bolt.set", "um_vet_assault_inferno.set", "um_vet_assault_plasma.set",
    "um_vet_assault_sgt.set", "bra_assault.set", "bra_assault_sgt.set",
]
RE_TAGS = re.compile(rb'^(\t\{tags (?:"[^"\r\n]*" ?)+)\}(\r?)$', re.M)


def remove(apply):
    """--remove (Alex, Nov 21 2026: "remove the melee tag off the assault marines, I don't want just the one zerging forwards"):
    takes the tag out of the {tags} line again, in place (no backup restore: other sessions edit these breeds)."""
    done = 0
    for name in FILES:
        p = DIR + "/" + name
        b = open(p, "rb").read()
        m = RE_TAGS.search(b)
        if not m or len(RE_TAGS.findall(b)) != 1:
            print(f"SKIP {name}: tags line not found exactly once")
            continue
        line = m.group(0)
        if b" " + TAG not in line:
            print(f"not tagged {name}")
            continue
        new_line = line.replace(b" " + TAG, b"", 1)
        new = b[:m.start()] + new_line + b[m.end():]
        assert new.count(b"\n") == b.count(b"\n") and new.count(b"\r\n") == b.count(b"\r\n") and TAG not in new
        print(f"{'edit ' if apply else 'would'} {name}:{b[:m.start()].count(b'\n') + 1}  {new_line.decode().strip()}")
        if apply:
            open(p, "wb").write(new)
            done += 1
    print(f"{done} file(s) written" if apply else "dry run: nothing written (--apply)")


def main():
    apply = "--apply" in sys.argv
    if "--remove" in sys.argv:
        return remove(apply)
    done = 0
    for name in FILES:
        p = DIR + "/" + name
        b = open(p, "rb").read()
        hits = RE_TAGS.findall(b)
        if len(hits) != 1:
            print(f"SKIP {name}: {len(hits)} tags line(s)")
            continue
        if TAG in b:
            print(f"has it {name}")
            continue
        new = RE_TAGS.sub(lambda m: m.group(1) + b" " + TAG + b"}" + m.group(2), b, count=1)
        assert new.count(b"\n") == b.count(b"\n") and new.count(b"\r\n") == b.count(b"\r\n") and len(new) == len(b) + len(TAG) + 1
        line_no = b[:RE_TAGS.search(b).start()].count(b"\n") + 1
        print(f"{'edit ' if apply else 'would'} {name}:{line_no}  {RE_TAGS.search(new).group(0).decode().strip()}")
        if apply:
            os.makedirs(BAK, exist_ok=True)
            if not os.path.exists(BAK + "/" + name):
                shutil.copy2(p, BAK + "/" + name)
            open(p, "wb").write(new)
            done += 1
    print(f"{done} file(s) written" if apply else "dry run: nothing written (--apply)")


if __name__ == "__main__":
    main()
