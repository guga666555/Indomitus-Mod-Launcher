# GEM SupplyTags 0.1

Switches an entity's supply zone off with an entity tag.

## Rule

- While an entity with an `{extender "supply_zone"}` (ammo crates, supply trucks / trailers, the Space Marine supply
  drop pod) carries the tag `supply_off`, its zone resupplies nobody and reports itself as disabled.
- Remove the tag and the zone works again at once (next 0.5 s tick).
- Plain entity tags: `{tags add "supply_off"}` / `{tags remove "supply_off"}` from script, breed tags, triggers.
  Tags are read on every tick, so they can change at any time during a match.
- Without the DLL the tag does nothing: the zone just works all the time (vanilla behaviour).

Indomitus use (Nov 21 2026): the supply drop pod `drop_pod_ammo` carries `supply_off` from spawn until its doors open
(`set\interaction_entity\40k_spawners\astartes.inc`, block `{"drop_pod_ammo"}`), so it does not resupply units while it
waits hidden or falls.

## How it works (game 1.069)

The engine has no script action for this. The supply zone extender (`eSupplyZoneExtender`, RTTI
`.?AVeSupplyZoneExtender@Entity@@`) has a reflected property `enabled` (byte at +0x430, default 1, saved in map files as
`{Extender "supply_zone" {enabled} {current 0}}`) that only the map / save loader writes. Its message handler
(vtable slot 7) runs the resupply pass on the tick message (0xB, every 0.5 s) and answers the status query (0xC); both
do nothing / report "cannot supply" when `enabled` is 0.

The DLL replaces vtable slot 7. For a tick or query on a zone whose owner entity (+0x18) carries the tag, it sets the
byte to 0 for the length of that one call and puts the engine's value back right after. So:

- nothing is held between calls: a save taken while the tag is on stores the real value, and a loaded game needs no
  restore step;
- a zone switched off in the map file stays off;
- all other messages (init etc.) are left alone.

Nothing is hard-coded: the vtable comes from the RTTI name, the message slot from its prologue, the owner offset from
the init branch of that slot, the `enabled` offset from the property registration next to the string `"enabled"`, and
the entity tag container from the `{tags}` action (same finder as GEM SensorTags / TeamTags). A failed finder leaves the
DLL off and logs why.

## Settings (`gem_supplytags.ini`)

| Key | Default | Meaning |
|---|---|---|
| Enabled | 1 | 0 = do nothing |
| Tag | supply_off | the tag (lowercase) |
| Log | 0 | 1 = log held zones (one line per 120 held ticks) |

Log: `gem_supplytags.log` (editor `gem_supplytags_editor.log`, server `gem_supplytags_server.log`) next to the DLL.
The first line says whether it installed and the offsets it found.

## Multiplayer

Resupply runs on the host, so the host (game or `call_to_arms_server.exe`) needs it. Clients only draw the zone;
running it everywhere through the payload is fine.

## Tested

Offline (`build.bat test`, output in `test_out.txt`): finders on the 1.069 game, editor and server exes (all three:
vtable, slot 7, enabled +0x430, owner +0x18, tags +0x1D0) and the hold logic on fake objects (8 cases ALL OK).
Not yet tested in a running game.

## Build

`source\build.bat` (zig cc; `ZIG_EXE` defaults to the launcher clone's `zig\zig.exe`) -> `out\gem_supplytags.dll`.
`build.bat test` -> `out\sp_test.exe` run on the three exes. `inject.bat` + `gem_inject.exe` = in-house test without
the launcher (inject from the main menu, before loading a map).
