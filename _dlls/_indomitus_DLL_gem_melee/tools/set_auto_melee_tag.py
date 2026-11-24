"""Nov 21 2026 (Alex: "do both"): [MeleeStance] AutoMeleeTag = melee_infantry,melee_auto.

melee_infantry = may melee outside the stance + the AI [Zerg] charge (the 6 TG melee breeds); melee_auto = may melee only (the
12 assault marine breeds, tools/tag_assault_marines.py).

1. gem_melee: the value and its note in tools/make_ini.py (the generator's table), then `python tools/make_ini.py` rewrites
   src/gem_melee.ini; README / spec lines that state the value.
2. MeleeStance's own source ini in the mc054 worktree + the dist copy next to it, so a standalone 0.7.42 build knows the word too.

Dry run by default; --apply writes. Every replacement must match exactly once. Backups of the mc054 inis:
Documents/Indomitus_backups/_tools/gem_melee/bak_auto_melee_tag/.
"""
import io
import os
import shutil
import sys

GM = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MC = "C:/Users/alex_/Documents/IndomitusLauncher_mc054/_dlls/_indomitus_DLL_meleestance"
BAK = "C:/Users/alex_/Documents/Indomitus_backups/_tools/gem_melee/bak_auto_melee_tag"

TEXT_EDITS = [
    ("tools/make_ini.py", [
        ('# Q1 AutoMeleeTag=melee_infantry, Q2 ProbeAnyRange=0 and CooldownMs=2000).',
         '# Q1 AutoMeleeTag=melee_infantry,melee_auto (Alex, Nov 21 2026), Q2 ProbeAnyRange=0 and CooldownMs=2000).'),
        ('("AutoMeleeTag", "melee_infantry"),', '("AutoMeleeTag", "melee_infantry,melee_auto"),'),
        ('''        "gem_melee 0.1.0: shipped as melee_infantry on purpose (Alex's Nov 20 rule; open question Q1). The code default is the same",
        "word, so a gem_melee.ini without this key turns the rule on too."],''',
         '''        "gem_melee 0.1.0 (Alex, Nov 21 2026): melee_infantry = may melee + the AI [Zerg] charge (the 6 TG melee breeds); melee_auto =",
        "may melee only, no [Zerg] (the 12 assault marine breeds: um_assault*, um_vet_assault*, bra_assault*). The code default is",
        "melee_infantry alone, so a gem_melee.ini without this key drops melee_auto: keep this key."],'''),
    ]),
    ("src/README.md", [
        ("- `[MeleeStance] AutoMeleeTag=melee_infantry`: written out on purpose.",
         "- `[MeleeStance] AutoMeleeTag=melee_infantry,melee_auto` (Alex, Nov 21 2026: `melee_auto` = may melee without the `[Zerg]`\n"
         "  charge, carried by the 12 assault marine breeds): written out on purpose."),
        ("- Q1 AutoMeleeTag: shipped as `melee_infantry` (0.7.42 default, Alex's Nov 20 rule; orchestrator decision).",
         "- Q1 AutoMeleeTag: shipped as `melee_infantry,melee_auto` (Alex, Nov 21 2026). `melee_auto` is on the 12 assault marine breeds\n"
         "  (they draw at fight range as bots, no 80 m charge; the thunder-hammer veteran now spawns pistol in hand). Before that:\n"
         "  shipped as `melee_infantry` (0.7.42 default, Alex's Nov 20 rule; orchestrator decision)."),
    ]),
    ("design/spec.md", [
        ("- Q1 AutoMeleeTag: ship `melee_infantry` (0.7.42 default, only 6 breed files carry the tag, OB-3) or empty (0.7.41 behaviour)?",
         "- Q1 AutoMeleeTag: ship `melee_infantry` (0.7.42 default, only 6 breed files carry the tag, OB-3) or empty (0.7.41 behaviour)?\n"
         "  DECIDED (Alex, Nov 21 2026): `AutoMeleeTag=melee_infantry,melee_auto`; the 12 assault marine breeds carry `melee_auto`\n"
         "  (may melee, no `[Zerg]` charge). Ini value only (tools\\make_ini.py table); no code change."),
    ]),
]
MC_OLD = b"\r\nAutoMeleeTag=melee_infantry\r\n"
MC_NEW = (b"\r\n; Nov 21 2026 (Alex): melee_auto = may melee without the [Zerg] charge; the 12 assault marine breeds carry it."
          b"\r\nAutoMeleeTag=melee_infantry,melee_auto\r\n")


def main():
    apply = "--apply" in sys.argv
    for rel, edits in TEXT_EDITS:
        p = os.path.join(GM, rel)
        s = io.open(p, encoding="utf-8", newline="").read()
        crlf = "\r\n" in s
        for old, new in edits:
            if crlf:
                old, new = old.replace("\n", "\r\n"), new.replace("\n", "\r\n")
            if new in s:
                print("has it :", rel, "|", new[:60].replace("\r\n", " "))
                continue
            assert s.count(old) == 1, (rel, old[:70], s.count(old))
            s = s.replace(old, new)
            print(("edit   :" if apply else "would  :"), rel, "|", new[:70].replace("\r\n", " "))
        if apply:
            io.open(p, "w", encoding="utf-8", newline="").write(s)
    for rel in ("src/gem_meleestance.ini", "dist/gem_meleestance.ini"):
        p = MC + "/" + rel
        b = open(p, "rb").read()
        if b"AutoMeleeTag=melee_infantry,melee_auto\r\n" in b:
            print("has it :", p)
            continue
        assert b.count(MC_OLD) == 1, (p, b.count(MC_OLD))
        print(("edit   :" if apply else "would  :"), p)
        if apply:
            os.makedirs(BAK, exist_ok=True)
            dst = BAK + "/" + rel.replace("/", "_")
            if not os.path.exists(dst):
                shutil.copy2(p, dst)
            open(p, "wb").write(b.replace(MC_OLD, MC_NEW))
    if not apply:
        print("dry run: nothing written (--apply)")


if __name__ == "__main__":
    main()
