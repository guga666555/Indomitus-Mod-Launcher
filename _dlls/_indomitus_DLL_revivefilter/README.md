# GEM ReviveFilter 0.1

Limits who may revive a downed soldier. Same idea as GEM BoardFilter and GEM RepairFilter.

## Rule

- A soldier with a tag or def prop `revive_by_<class>` can only be revived by units with the tag `<UnitPrefix><class>`
  (UnitPrefix is empty in the shipped ini, so the unit tag is the class itself). Several classes are allowed on
  both sides; one match is enough.
- A soldier with no `revive_by_` tag: anyone may revive him (vanilla).
- Tags come from the breed `{tags}` (GEM BreedTags copies them onto the unit) or from script `{tags add}`. They are
  read on every check.

Indomitus (Nov 17 2026): the 147 breeds with `"boarder_astartes"` (SM + CSM power armour; scouts are not) carry
`"revive_by_apothecary"`; `um_apothecary` and `bra_apothecary` carry `"apothecary"`. So a Guard medic cannot revive
a marine, an Apothecary revives marines and Guardsmen. Written by `apply_apothecary.py`. No CSM breed has the
`apothecary` tag yet.

## What happens to a refused unit

- Player command (revive click on a downed soldier): the order is not given, and the dll fires the script event
  `revive_refused` on the downed soldier (once per 3 s per unit + patient pair). The `"human"` block in
  `40k_human.inc` shows "This action requires an Apothecary". The click runs on the clicking player's machine
  only, so only that player sees it.
- Any other revive order (auto-medic, script): the order fails on its first update. No popup.

## Settings (`gem_revivefilter.ini`)

| Key | Default | Meaning |
|---|---|---|
| Enabled | 1 | 0 = do nothing |
| PatientPrefix | revive_by_ | downed soldier's tag / def prop prefix |
| UnitPrefix | (empty) | reviver's tag prefix; empty = the class name itself |
| Command | 1 | 0 = leave the player's command alone, only fail the order |
| PopupEvent | revive_refused | script event fired on the patient on a refused command (empty = none) |
| PopupCooldownMs | 3000 | one popup per unit + patient pair in this time |
| Server | 1 | also patch the dedicated server exe |
| Log | 0 | 1 = log the first 300 refusals |

Log: `gem_revivefilter.log` (editor `_editor.log`, server `_server.log`) next to the DLL.

## Multiplayer

The revive order runs in the simulation, so every player and the server need the DLL with the same ini.

## Build

`build.bat` (zig from `..\..\zig`, PATH or `python -m ziglang`); `build.bat payload` copies the dll + ini into `launcher\payload`. `build.bat test` runs the finders on the three 1.068
exes and the rule, both wrappers and the popup cooldown on fake objects.

Engine notes: `_notes\dll_hooks.md`, section "Revive filter".
