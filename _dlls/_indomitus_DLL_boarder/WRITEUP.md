# GEM BoardFilter 0.1 — writeup

Call to Arms: Gates of Hell 1.067 (game, editor, dedicated server). Plain DLL, no proxy. Tested by injection into the
editor on Nov 6 2026: a Space Marine ordered into an untagged Imperial Guard vehicle was refused and the
"CANNOT BOARD THIS VEHICLE!" popup showed over the vehicle.

## What it does

Lets the mod decide **who may board which vehicle** with entity script tags. Indomitus uses it so that:

- Space Marines and Chaos Space Marines (power armour) can't crew or ride Imperial/Traitor Guard vehicles,
- Guardsmen of either side can't board daemon engines or (future) Astartes vehicles,
- any vehicle can be opened or closed to anyone at runtime from script.

The engine has nothing like this: seats have no tag filter, and the human `able crew` flag blocks every vehicle at once.

## The rule

| Tag | On | Meaning |
|---|---|---|
| `board_<class>` | vehicle | units of `<class>` may board it (any number of these tags) |
| no `board_` tag | vehicle | treated as `board_mortal` (ini `DefaultBoard`) |
| `no_board` | vehicle | nobody may board it |
| `boarder_<class>` | unit | the unit's class (any number) |
| no `boarder_` tag | unit | class `mortal` (ini `DefaultClass`) |

A unit may board when it has a class the vehicle lists. Tags are read on every check, so script changes such as
`{tags add "no_board"}` take effect immediately. Prefixes, the no-board tag and the defaults are ini settings.

Mod side (Indomitus, not in this package): SM/CSM breeds carry `boarder_astartes` in their breed `{tags}` (copied to the
entity by GEM BreedTags, so **BreedTags must be loaded too**); daemon engines carry def prop `board_daemon`; small blocks in
`set\interaction_entity\40k_vehicle\40k_vehicle_main.inc` turn the def props `board_mortal / board_astartes / board_daemon /
no_board` into tags on spawn, and the `"vehicle"` block's `{on "board_refused"}` shows the popup.

## What the player sees

- **Ordering a unit into a vehicle it may not board**: the unit doesn't move, the order ends at once (engine log line
  "Unable to board …"), and a red popup appears over the vehicle. The popup is throttled to once per 3 s per unit+vehicle
  pair. The board cursor still shows: the engine's cursor check only knows the local player, not the selected units.
- **A unit already sitting in a vehicle it may not board** (map-placed crew, or tags changed at runtime) is thrown out by the
  engine's own "wrong crew" emit.
- **AI**: a refused order ends before the unit walks anywhere, so the AI cannot get stuck walking back and forth. If the
  engine AI ever keeps re-ordering the same refused vehicle, the log says
  `order retry: unit … -> vehicle … refused N times in a row`. Not seen in the editor test; one player click produces two
  checks, which is normal.

## How it works (engine side)

Offsets are for the game exe 1.067; the DLL finds everything by byte pattern at runtime and the same code exists in the editor
and server builds (offsets listed at the end).

1. **Crew check core** `exe+0x7FA9D0` `CanCrew(vehicle, playerByte*)`: vanilla script tag `abandoned` → false; the
   vehicle's placer must have places; owner/relation check with the actor ables `any_crew` / `ally_crew`. It reads the
   vehicle's script-tag vector at `entity+0x4E8` — the DLL takes that offset from this code instead of hard-coding it.
2. Two wrappers call it and are the ones that know the unit:
   - **A** `exe+0x7FA7E0 (vehicle, human)` — called by the board order (`eOrderBoard.cpp`) when the order starts and again
     when the unit arrives. False ends the order.
   - **B** `exe+0x7FA9A0 (vehicle, entity)` — called by the vehicle-state loop that walks every occupied place
     (place stride 0x298, occupant at `+0x280`). False makes the engine emit that occupant.
3. The DLL overwrites the first 14 bytes of A and the first 16 bytes of B with an absolute jump to a stub it generates at
   install time. The stub calls the tag check. On refusal it returns `al = 0`; otherwise it replays the overwritten
   instructions (the `je` inside them re-targeted with an absolute jump) and jumps back into the original function. Nothing
   else in the engine changes.
4. **Popup event**: on an order refusal the DLL fires script event `board_refused` on the vehicle, exactly the way the engine
   fires `throw_off_end`: Tag constructor (`exe+0x556310`, called lazily on first use — not in DllMain, because injected
   before start the engine's statics aren't initialised), context constructor `exe+0x76DCD0 (buf, entity)`, fire
   `exe+0x76E0B0 (ctx, &tag)`, context destructor `exe+0x76DD60`. Found from the `throw_off_end` string's code site.
5. **Throttle clock**: the engine's sim clock (global uint32 ms at `exe+0x1556E48`, read by the AI brain timers), so every
   machine makes the same decision.

## Multiplayer

Boarding is simulated in lockstep, and the check (and the popup event) runs on every machine, so **every player must run the
same DLL and ini**, like the other GEM hooks. The rule only reads tags and the sim clock, so all machines agree.

## Install / integration

- Ship `gem_boardfilter.dll` with `gem_boardfilter.ini` next to it (the DLL reads `<its own name>.ini`).
- Load before or after the engine starts; hooks install in `DllMain`. If it is already running and you inject again, the
  finders fail ("not found … already patched") and nothing is installed twice.
- Do **not** unload it while the game runs (the patched entry points jump into it). Uninstall = close the game / remove it
  from the launcher payload.
- Log: `gem_boardfilter.log` (`_editor` / `_server` suffix) next to the DLL. First line = install result + sites; set
  `Log=0` to silence refusals.
- Needs GEM BreedTags for the unit tags (`boarder_*` live in breed `{tags}`).

## Ini

```
[BoardFilter]
Enabled=1
VehiclePrefix=board_
UnitPrefix=boarder_
NoBoardTag=no_board
DefaultBoard=mortal        ; classes accepted by a vehicle with no board_ tag (comma list)
DefaultClass=mortal        ; class of a unit with no boarder_ tag (comma list)
EjectOccupants=1           ; 0 = don't emit units already inside
PopupEvent=board_refused   ; empty = no event
PopupCooldownMs=3000
Log=1
```

## Sites per build

| | game | editor | server |
|---|---|---|---|
| A (order) | 0x7FA7E0 | 0x67C730 | 0x610420 |
| B (seat) | 0x7FA9A0 | 0x67C8F0 | 0x6105E0 |
| core | 0x7FA9D0 | 0x67C920 | 0x610610 |
| tag ctor / ctx / fire / dtor | 0x556310 / 0x76DCD0 / 0x76E0B0 / 0x76DD60 | 0x3DD9F0 / 0x5EC280 / 0x5EC6C0 / 0x5EC320 | 0x3716D0 / 0x583D60 / 0x584140 / 0x583DF0 |
| sim clock | 0x1556E48 | 0x13BE208 | 0x11A1328 |

Entity tags at `+0x4E8` in all three.

## Not covered / open

- Static guns: not checked whether manning a gun (`board_cannon`) goes through the same check.
- Who sees the `Damage_Report` popup (only the unit's owner or everyone) is not checked.
- The board cursor is not filtered.

## Build

`build.bat` (zig cc: `pip install ziglang`) → `out\`. `build.bat test` → runs the finders on all three exes, builds the stubs,
and checks 14 rule cases + the popup throttle (ALL OK on 1.067).
