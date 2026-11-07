# GEM BoardFilter 0.3

Restricts who may crew / board a vehicle with **def props**. Call to Arms: Gates of Hell 1.067 (game, editor, server).
Indomitus Launcher payload dll (`gem_boardfilter.dll` + `gem_boardfilter.ini`). 0.1 by Alex; 0.2 launcher adaptation;
0.3 the `crew_only_` rule. Engine side in depth: `WRITEUP.md` (written for 0.1: its rule section is the old board_/boarder_
scheme, everything about hooks, popup and engine sites still holds).

## Rule

| Where | What | Meaning |
|---|---|---|
| vehicle `.def` | `{props "crew_only_<tag>"}` (any number) | only humans with one of those `<tag>`s may board it |
| vehicle `.def` | no `crew_only_` prop | anyone may board (vanilla) |
| human | its engine tags + its def `{props}` | checked against the vehicle's list |

Matching is exact and case-sensitive, like the engine's own tag checks. Only the vehicle's def **props** count (a runtime tag
`crew_only_x` on a vehicle is ignored). Non-humans are never filtered.

Example: a human tagged `astartes` and a vehicle with `{props "crew_only_gretchin"}` → refused. The vehicle with
`{props "crew_only_gretchin" "crew_only_astartes"}` → accepted; a `guard` human → refused.

Human tags come from the breed `{tags}` (made real entity tags by GEM BreedTags, which is in the payload), script
`{tags add "..."}` (live), or the human's own `.def` `{props}`. Nothing needs converting in `40k_vehicle_main.inc` any more; that
file only needs the `{on "board_refused"}` popup block. The ini prefix is `PropPrefix=crew_only_`.

## What happens in game

- **Board order** (player or AI): the engine's board order (eOrderBoard) asks the crew check when the order starts and again on
  arrival. A refusal ends the order (engine log "Unable to board"). The board cursor itself is not filtered (it only knows the
  local player, not the unit), so the cursor still shows, but the unit doesn't board.
- **Popup**: on an order refusal the dll fires script event `board_refused` (ini `PopupEvent`) on the vehicle, the way the
  engine fires `throw_off_end`. `40k_vehicle_main.inc` `"vehicle"` shows `{Damage_Report "basis" "...CANNOT BOARD THIS VEHICLE!"}`.
  Throttled per unit+vehicle pair (`PopupCooldownMs` 3000) on the engine sim clock (same on every machine, MP stays in lockstep).
- **AI retries**: a refused order ends at its start, before the unit walks anywhere. The log reports
  `order retry: ... refused N times in a row` (N = 5, 21, 101, every 1000) when the same unit keeps being refused at the same
  vehicle within 10 s (one click = 2 checks, that's normal).
- **Already inside** (`EjectOccupants=1`): a refused occupant is emitted by the engine's own "wrong crew" path. Covers
  map-placed crews and tags changed at runtime.

## In the Indomitus Launcher

- `build.bat payload` builds and copies `gem_boardfilter.dll` + `.ini` into `..\..\launcher\payload\`; then rebuild the launcher
  (`build_zig.bat`). Loads alphabetically after gem_bombbay, before gem_breedtags (it only reads tags at check time, so the order
  doesn't matter).
- Log: `%LocalAppData%\Indomitus Launcher\payload\<code>\gem_boardfilter[_editor|_server].log`. First lines = settings, install
  result + sites, popup event state.
- Only acts inside `call_to_arms*.exe` (not the workshop tool); one copy per process (a second injected copy logs "another copy
  is already loaded" and stays off); pins itself once patched (it must never unload: the patched entries jump into it).
- No other payload dll patches the crew check (A, B or core).
- **Multiplayer**: boarding is simulated in lockstep, so every player must run the same dll and ini - which the sealed launcher
  guarantees.

## Engine (game exe 1.067; found by byte pattern, same code in editor/server)

| | game | editor | server |
|---|---|---|---|
| A (order) | 0x7FA7E0 | 0x67C730 | 0x610420 |
| B (seat) | 0x7FA9A0 | 0x67C8F0 | 0x6105E0 |
| core | 0x7FA9D0 | 0x67C920 | 0x610610 |
| tag ctor / ctx / fire / dtor | 0x556310 / 0x76DCD0 / 0x76E0B0 / 0x76DD60 | 0x3DD9F0 / 0x5EC280 / 0x5EC6C0 / 0x5EC320 | 0x3716D0 / 0x583D60 / 0x584140 / 0x583DF0 |
| sim clock | 0x1556E48 | 0x13BE208 | 0x11A1328 |

Entity runtime tags at `+0x4E8`, def `+0x548`, def props `+0x330` in all three (taken from the engine's own code). The dll replaces the first 14 (A) / 16 (B) bytes with a
jump to a stub: check → refusal returns false, otherwise the replaced instructions run (the `je` re-targeted) and execution
continues in the original.

## 0.3 changes (2026-11-06)

- `crew_only_<tag>` vehicle def props replace board_/boarder_/no_board/DefaultBoard/DefaultClass (no default class any more:
  a vehicle without the prop is unrestricted). Human side matches runtime tags and def props, exact match.
- Def props read directly: def `[entity+0x548]`, props vector `[def+0x330..0x338]` (0x60-byte Tags, name at +0x40), the same
  list the engine's `{if prop}` reads. Offsets voted from the engine's ~70-90 def-props loads (needs ≥ 20 votes and twice the
  runner-up, the FSM state vector); logged on the install line.
- Up to 32 `crew_only_` props per vehicle are honoured; tag names up to 63 chars.

## 0.2 changes (launcher adaptation, 2026-11-06)

- Game-exe gate, one-copy-per-process mutex (`Local\gem_boardfilter_<pid>`), self-pin after patching.
- Unicode ini/log paths (`GetPrivateProfile*W`, `_wfopen`); ini values clamped (`PopupCooldownMs` 0..600000).
- **Fixed:** the refusal log line and the install log line could overflow their 256-byte stack buffers with many / long
  classes (`strncat(..., 200 - strlen)` wraps once the line passes 200 chars). Now a bounded append.
- Refusal bookkeeping (counters, popup pair table, log) under an SRW lock; the script event fires outside it, so a handler that
  re-enters a board check can't deadlock. The allowed path takes no lock.
- Stub page is made execute-read after writing (was RWX).
- Test: runs on Linux too (`test/run_host_test.sh`), and now executes the stubs against the mapped exe: refusal, pass-through
  into the real crew check and its tail call, the engine's own refusal, the original NULL-unit branch, `EjectOccupants=0`.
- Rule logic (in 0.2), finders and stub bytes unchanged from 0.1.

## Files

- `src\gem_boardfilter.c`, `src\gem_boardfilter.ini` (the payload copy), `build.bat`, `test\` (Linux offline check).
- `src\gem_inject.c` + `inject.bat`: in-house runtime injector for quick editor tests (`build.bat inject`); not shipped.
- `bin\`, `tools\`: the 0.1 binaries as received (git-ignored: `*.dll`, `*.exe`).

## Build / test

`build.bat` → `dist\`. `build.bat test` → finders + tag / def-props offsets on all three exes, stub run-through, 18 rule
cases, popup throttle, log bound → `ALL OK` (checked offline on 1.067, Nov 6 2026). `build.bat payload` → copies into the launcher payload.
