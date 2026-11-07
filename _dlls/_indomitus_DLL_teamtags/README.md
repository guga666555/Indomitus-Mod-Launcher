# GEM TeamTags 0.1

Every unit (human, vehicle, gun) gets the entity tag `team<N>`, where N is its owning player
(the number `{player N}` sets): player 0 = `team0`, player 1 = `team1`, and so on up to `team17`.
No mission triggers, no spawn defines needed. Indomitus removed its `team_checker` trigger loop on
Nov 7 2026, so the mod's team checks (`40k_team.inc`: auras, melee contactors) now rely on this dll.

- Set when the unit gets its player: spawn, `{player N}`, purchase, call-in, capture, crew handoff.
- When the owner changes, the old `teamX` tag is removed and the new one added.
- Re-applied after a unit is loaded from a map or save (loading sets the player before the saved tags).
- Units with no player keep their tags as they are.
- They are normal entity tags: `{if tagged "team1"}`, trigger `{tag "team1"}`, `{tags remove}` all work.
  A script that changes a team tag by hand is overruled the next time the owner changes or the map loads.
- Tags are per player, not per battle side: two allied players have different team tags.

Tested in game Nov 7 2026 (Alex): battle zones (`1v1_radar_hill`) and conquest (`dcg_hungarian_swamps`).

## Files
| File | |
|---|---|
| `src\gem_teamtags.c` | source (dll + the `TT_TEST` offline test) |
| `src\gem_teamtags.ini` | `Enabled`, `Prefix` (default `team`), `ReapplyOnLoad`, `Log` |
| `build.bat` | `build.bat` / `build.bat payload` / `build.bat test [bin folder]` |

The dll writes `gem_teamtags.log` next to itself (`_editor` / `_server` suffix for those exes); with the
launcher that is `%LocalAppData%\Indomitus Launcher\payload\<code>\`. Set `Log=1` to log every tag change.

## Multiplayer
The owner is simulation state and runs on every machine, and the tags are read by simulation scripts.
Every player must run the dll (the launcher payload does that). Otherwise one machine has the tags and
another does not.

## How it works (game 1.067, everything found at startup)
- `eActor::SetPlayer(entity, uint8_t*)` exe+0x7EEEC0 (editor +0x6710C0, server +0x605460). This is the only
  code that writes the owner byte `+0xDA8` (the constructor writes 0xFF = none). It is virtual only (slot
  +0x1D0 in the eActor and eHuman vtables), with no direct callers in any of the three exes. The dll patches
  both slots: run the original, then fix the tags. Found through `eAction_Player` (RTTI): its Execute calls
  slot +0x1D0 after comparing the player byte.
- `eActor::Unpack` exe+0x7F6470 / `eHuman` override exe+0x95BD30 (vtable slot +0x238, map/save load,
  logs "Changing mid for %s failed"). It sets the player before the saved tags are read. The dll skips
  SetPlayer while inside Unpack, then applies the tag once Unpack returns.
- Tag changes use the `{tags add}` action's own engine path: copy container, `remove_all(team0..team31)`,
  `add_all(teamN)`, `Entity::SetTags`, dtor. The two containers are built once with the engine's
  TagContainer ctor + string parser (`FromString` exe+0x558040) and checked for the expected tag count.
  Units that already have the right tag and no other team tag are skipped (no SetTags call).
- No other payload dll patches these vtable slots.

Offline test: `build.bat test` runs the finders on game, editor and server (ALL OK on 1.067) plus the
tag-scan logic.
