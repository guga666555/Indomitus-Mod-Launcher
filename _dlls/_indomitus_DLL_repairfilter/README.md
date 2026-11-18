# GEM RepairFilter 0.2

Limits who may repair a vehicle.

## Rule

- A vehicle with a tag or def prop `repair_by_<class>` can only be repaired by units with the tag `<UnitPrefix><class>`
  (UnitPrefix is empty in the shipped ini, so the unit tag is the class itself). Several classes are allowed on
  both sides; one match is enough.
- A vehicle with no `repair_by_` tag or prop: anyone may repair it (vanilla).
- Unit tags come from the breed `{tags}` (GEM BreedTags copies them onto the unit) or from script `{tags add}`.
  Tags are read on every check, so script can add or remove them during a match.

Indomitus (Nov 17 2026): superheavies (every def with prop `superheavy`, plus Rogal Dorns, Malcadors, Gorgon,
Minotaur; not the Ragnarok) and the 12 dreadnoughts have `"repair_by_mechanicus"`; the IG / TG tech-priests, the
techmarines and the Master of the Forge carry the breed tag `"mechanicus"`. Written by `apply_mechanicus.py`.

The vanilla rules still apply first: the repairer needs `able repair` and a repair kit, the vehicle `able repaired`.

## What happens to a refused unit

- Player command (right-click repair): the order is not given. The unit keeps doing what it was doing, the same as
  a unit that cannot repair at all, and the dll fires the script event `repair_refused` on the vehicle (once per
  3 s per unit + vehicle pair). The mod's `"vehicle"` block in `40k_vehicle_main.inc` shows a red Damage_Report
  for it, like GEM BoardFilter's `board_refused`. The click runs on the clicking player's machine only, so only
  that player sees it.
- Any other repair order (a crew leaving its vehicle to repair it, script, AI): the order fails on its first
  update, the same way it fails when the target vehicle is gone. No popup.

## Settings (`gem_repairfilter.ini`)

| Key | Default | Meaning |
|---|---|---|
| Enabled | 1 | 0 = do nothing |
| VehiclePrefix | repair_by_ | vehicle side tag / def prop prefix |
| UnitPrefix | (empty) | unit side tag prefix; empty = the class name itself |
| Command | 1 | 0 = leave the player's command alone, only fail the order |
| PopupEvent | repair_refused | script event fired on the vehicle on a refused command (empty = none) |
| PopupCooldownMs | 3000 | one popup per unit + vehicle pair in this time |
| Server | 1 | also patch the dedicated server exe |
| Log | 0 | 1 = log the first 300 refusals |

Log: `gem_repairfilter.log` (editor `gem_repairfilter_editor.log`, server `gem_repairfilter_server.log`) next to the
DLL. The first line says whether it installed, the offsets it read and whether the popup event was found.

## Multiplayer

The repair order runs in the simulation, so every player and the server need the DLL with the same ini.

## Build

`build.bat` (zig from `..\..\zig`, PATH or `python -m ziglang`); `build.bat payload` copies the dll + ini into `launcher\payload`. `build.bat test` runs the finders (incl. the popup
event) on the three 1.068 exes (one exe per process) and the rule, both wrappers and the popup cooldown on fake
objects.

Engine notes: `_notes\dll_hooks.md`, section "Repair filter".
