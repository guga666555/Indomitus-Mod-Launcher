"""Nov 21 2026 (Alex: "do both"): um_vet_assault_plasma spawns with its plasma pistol in hand, like the other assault marine
breeds, instead of the thunder hammer. `{in_hands 0}` = the first inventory item, so the pistol + its ammo move in front of the
hammer; nothing else changes. Reason: a breed that may melee outside the stance (AutoMeleeTag) and holds a melee weapon charges
infantry it sees ([Charge], and [Zerg] with melee_infantry), so this one breed would have run forward alone.

Dry run by default; --apply writes. CRLF kept; backup in Documents/Indomitus_backups/_tools/gem_melee/bak_assault_tags/.
"""
import os
import shutil
import sys

P = ("C:/Program Files (x86)/Steam/steamapps/common/Call to Arms - Gates of Hell/mods/Indomitus_DEV"
     "/resource/set/breed/mp/sm/warhammer/um_vet_assault_plasma.set")
BAK = "C:/Users/alex_/Documents/Indomitus_backups/_tools/gem_melee/bak_assault_tags/um_vet_assault_plasma.set.before_pistol_in_hand"
OLD = (b'\t\t{item "sm_hammer_artificer" filled}\r\n'
       b'\r\n'
       b'\t\t{item "sm_plasmapistol_artificer" filled}\r\n'
       b'\t\t{item "plasmagun_mag ammo" 48}\r\n')
NEW = (b'\t\t{item "sm_plasmapistol_artificer" filled}\r\n'
       b'\t\t{item "plasmagun_mag ammo" 48}\r\n'
       b'\r\n'
       b'\t\t{item "sm_hammer_artificer" filled}\r\n')


def main():
    apply = "--apply" in sys.argv
    b = open(P, "rb").read()
    if NEW in b and OLD not in b:
        print("already pistol first")
        return
    if b.count(OLD) != 1:
        print("SKIP: the hammer / pistol block was not found exactly once (file changed?)")
        return
    out = b.replace(OLD, NEW)
    assert len(out) == len(b) and sorted(out.split(b"\r\n")) == sorted(b.split(b"\r\n"))    # same lines, another order
    first = out[out.index(b"{inventory"):].split(b"\r\n")[1].strip().decode()
    print(("edit " if apply else "would ") + os.path.basename(P) + ": first inventory item is now " + first)
    if apply:
        os.makedirs(os.path.dirname(BAK), exist_ok=True)
        if not os.path.exists(BAK):
            shutil.copy2(P, BAK)
        open(P, "wb").write(out)
    else:
        print("dry run: nothing written (--apply)")


if __name__ == "__main__":
    main()
