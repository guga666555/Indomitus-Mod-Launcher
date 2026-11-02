# GEM BreedTags 0.7 (test build)

**0.7:** breed tags only. The engine tag view (Alt+T in game) and the floating tag labels (F9) are removed - the DLL no
longer hooks the `{if tagged}` condition, polls keys or writes damage-report entries. The editor's own Alt+T still works
(that is the editor's, not this DLL's). ini keys `TagView*` / `ShowTags*` are gone, and so is the `RequireMod` mod gate (the launcher already only
starts Indomitus games with this DLL).

## 0.6 - re-breeds and late breeds (conquest / army preview)
Two engine behaviours made `{on spawn}`-based scripts (the skin/skeleton selector) miss the breed tags:
1. **Re-breed without respawn.** The preview keeps the same soldiers and gives them a new breed when you click
   through a squad (log: the same entity gets `nurgle_id`, then `blood_pact_id`, ...). No new `{on spawn}` fires.
2. **Breed applied on the unit's first update.** While a scene is being created, `SetBreed` stores the request and
   `eHuman::Quant` applies it later - after `{on spawn}` already ran.
And 0.5 only ever added tags, so a re-bred soldier carried both factions' tags.

0.6:
* `{on "breed_applied"}` - fired on the unit after every breed application (tags already merged), when the unit is
  in the scene. Run the selector from it too:
  ```
  {on spawn          {Delay 0.01 ("Skin_Prop_Selector")}}
  {on "breed_applied" {Delay 0.01 ("Skin_Prop_Selector")}}
  ```
  Without the DLL it never fires (quoted name = the engine's own custom-event syntax, no load error).
* On a re-breed the old breed's tags are removed before the new ones are added (`RemoveOldBreedTags=1`).
* Log per application: `breed "<name>" -> entity ... (at once | on update, thread main | <id>): removed [...] added
  [...], event fired | not fired (...)`.
* Changes simulation state like 0.5: every player of a multiplayer match must run the same DLL.



Makes a breed's `{tags "..."}` real entity tags the moment the breed is applied, so
`{if tagged "..."}` in interaction_entity scripts and `{tag ...}` trigger selectors see them.
No inventory items, no mission triggers - the tags are baked into the breed file:

```
{breed
	...
	{tags "soldier" "gem_default_tx_00" "commissar_ID" "gasmask_ID"}
	...
}
```

## Why a DLL

The engine keeps breed `{tags}` in a container on the breed object (`eHumanBreed.cpp`) and
never copies it to the entity's own tag container, which is what `tagged` / `{tags add}` use.
Scene queries (Lua `QueryScene`) read the breed copy, scripts read the entity copy. There is no
data-file key that bridges them, so this hooks `ApplyBreed(human, flags)` (`eHumanBreedUsage.cpp`)
and, after it returns, merges the breed's container into the entity's exactly the way
`{tags add}` does (copy -> add_all -> `Entity::SetTags` -> free). `SetTags` also registers the
entity in the global tagged-entity list that selectors iterate, and fires `{on "<tag>"}` handlers.

## When it is active
Always, in the game, editor and dedicated server, once loaded. The 0.5/0.6 `RequireMod` gate is gone (0.7): the
Indomitus Launcher only injects this DLL into games it starts with the Indomitus mods. If you install it as a plain
`dbghelp.dll` proxy instead, it is active in every game - remove it for vanilla play. `Enabled=0` switches it off.

## Install

Copy into `...\Call to Arms - Gates of Hell\binaries\x64\`:

| File | What it is |
|---|---|
| `dbghelp.dll` | this tool, as a dbghelp proxy (the exes import 6 dbghelp functions; they are forwarded to the real `System32\dbghelp.dll`) |
| `gem_breedtags.ini` | settings |

Works alongside the existing `version.dll` loader / Conquest+ / Editor+ - different proxy, no
interaction. Loads into `call_to_arms.exe`, `call_to_arms_ed.exe` and `call_to_arms_server.exe`;
other processes only get the pass-through.

Nothing is hard-coded per game version: every address is derived at startup from the exe
(string xrefs, the `{tags}` action's vtable, the breed parser). On a build it does not
recognise it logs `OFF` and patches nothing. Verified offline against 1.067
(game `6AB3DCC7`, editor `6AB3DD83`, server `6AB3DD21`).

`gem_breedtags.log` after a start:

```
GEM BreedTags 0.7 loaded into call_to_arms.exe (timestamp 6AB3DCC7)
ApplyBreed = exe+0x95CAD0, human->breed @ +0x1390
Breed tag container @ breed+0x128
TagContainer copy=exe+0x556DE0 add_all=exe+0x557060 remove_all=exe+0x557120 SetTags=exe+0x71A3F0 dtor=exe+0x556F00, entity container @ +0x1D0
ApplyBreed callers: exe+0x93A881 (on the unit's update), exe+0x95B9A0 (at once)
Breed tags -> entity tags      ON
Old breed tags on a re-breed   removed
{on "breed_applied"} event        ON (fired after every breed applied to a unit in the scene)
breed "..." -> entity ... (at once, thread ...): removed [-] added ["soldier" ...], event fired
```

**Uninstall:** delete `dbghelp.dll`. **Switch off:** `Enabled=0` in the ini.

For offline use. Every process that simulates the battle needs it (host, dedicated server,
every client in real multiplayer), or the simulations desync.

## Build

`build.bat` (Windows: portable zig next to it, the ScriptExt project's zig, zig on PATH or `python -m ziglang`) ->
`dist\gem_breedtags.dll`; for the Indomitus Launcher copy it + `gem_breedtags.ini` into `launcher\payload\`.
`source/build.sh` - the same with `pip install ziglang`. `gcc -DBT_TEST`
builds a Linux test harness that runs the address derivation against an exe without running it.
