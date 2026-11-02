# GEM ScriptExt 0.9.1

Engine extender for **Call to Arms: Gates of Hell** (GEM engine, game 1.067). It adds script words to
`interaction_entity` scripts: conditions, health actions, a damage multiplier and named events. One DLL works in the
game, the editor and the dedicated server.

The fauna features (health row in the vehicle panel, squad strip items, fauna squad leaders) were removed in 0.8.0;
they live in GEM Fauna now. The last ScriptExt source with them is `_bckp/0.7.0_with_fauna/`.

Every address is found at startup from strings, RTTI and byte patterns; nothing is hard-coded per exe. If the
engine code is not recognised (for example after a game update), that one feature stays off and the log says so.

## Contents

* [Install](#install)
* [Script words](#script-words)
* [Multiplayer](#multiplayer)
* [Settings and logs](#settings-and-logs)
* [Build and offline check](#build-and-offline-check)
* [Source layout](#source-layout)
* [How it works](#how-it-works)
* [Known limits and ideas](#known-limits-and-ideas)
* [Changelog](#changelog)

## Install

Same loader as GEM BreedTags.

* **Indomitus Launcher (recommended):** copy `dist\gem_scriptext.dll` and `gem_scriptext.ini` into
  `launcher\payload\` and rebuild the launcher. The launcher injects the DLL before the game starts; the file name
  does not matter.
* **dbghelp proxy (legacy):** copy `dist\gem_scriptext.dll` to `binaries\x64\dbghelp.dll`, plus `gem_scriptext.ini`.
  Only one dbghelp proxy can exist. The real `System32\dbghelp.dll` is loaded only when the file is named
  `dbghelp.dll`; injected by the launcher, ScriptExt never loads it.

## Script words

### Conditions

| Script | Meaning |
|---|---|
| `{if "have_enemy" ...}` | true while the unit's sensor currently sees at least one enemy. Quoted spelling: without the DLL it is an unset script flag (false, no error). |
| `{if have_enemy ...}` | same, unquoted. Logs `unknown condition` when the DLL is absent. |
| `{if health 0.6 ...}` | true while health is at or below 60% of max health (any unit with a health component). |
| `{if player 4 ...}` | true when the unit belongs to player 4. Players are numbered as in the editor (`player 0`, `player 1`, ...; 0 to 17). |
| `{if player ally ...}` | the unit belongs to the same player as the other unit of the event, or to an allied player (see below). |
| `{if player neutral ...}` | the unit is neutral to the other unit of the event (also: it belongs to no player). |
| `{if player enemy ...}` | the unit is an enemy of the other unit of the event. |
| `{if team 2 ...}` | true when the unit is in team 2 (see below). Also `{if team b}` / `{if team "b"}`. |

**Teams.** In a multiplayer battle (network game, or a battle with bots such as skirmish and Conquest) the team is the
battle's own team, which the game names `a`, `b`, ...: `a` = 1, `b` = 2 (the bot scripts' `side 'a'` / `'b'`). A game mode with another
team name (up to 7 letters) can be named directly, e.g. `{if team dm}`. Units of a player that is not in the battle's
player list (map-owned units) are in no team. Outside a multiplayer battle (single-player missions, the editor) there
are no such teams, so alliances decide: team 1 = player 0 and every player allied with it (both ways in the diplomacy),
team 2 = the lowest player not in team 1 and its allies, and so on (with no alliances set, every player is its own
team: player 0 = team 1, player 1 = team 2, ...). The log shows each player's team the first time a script asks.

```
{if team 2
    {if player 4 ...}       ; player 4 of team b
}
```

**Ally / neutral / enemy** use the engine's own relation check (the diplomacy, as `{if fogged}` uses it) between the
unit the condition is about and a reference:
* **an event with an effector** (hits, links, `{on fire ...}`, ...): the reference is the other unit of the event. The
  condition is about the script's unit, compared with the effector. Inside `{with_effector ...}` the engine swaps the
  two, so there the condition is about the effector, compared with the script's unit:
  ```
  {on "some_event"
      {with_effector
          {if player enemy  ...}   ; the effector is an enemy of this unit
          {if player ally   ...}   ; same player or allied
          {if player neutral ...}
      }
  }
  ```
  Result and meaning are the same on every PC, so these are safe in multiplayer.
* **an event without an effector**: the reference is the local player (the person playing on this PC; with no
  alliances set, `ally` means "my units"). In a network game each PC has its own local player, so there these
  results differ between PCs: use them only for local effects, not to change game state.
* A unit that the engine does not count as operatable (for example still being placed) is `neutral`.

### Actions (change game state, see [Multiplayer](#multiplayer))

| Script | Meaning |
|---|---|
| `{health_damage_count 0.25 max_health}` | damage = 0.25 x the unit's max health (exact, no damage-type scaling). Negative heals, overheal allowed. |
| `{health_damage_count -0.5 max_health 1}` | the optional 3rd argument `1` stops the heal at 100%. |
| `{health_damage_count 1.0 set_health}` | health = value x max health (`2.0` = 200%, `0` kills through the engine's normal death). |
| `{multiplier X}` inside `{health_damage_pierce ...}` / `{health_damage_blast ...}` | the unit takes X times that damage (default 1.0). |

Plain `{health_damage_count 25}` is unchanged. Every `{health_damage_count ...}` needs an `{effects ...}` block, as in
vanilla (`{effects "" "hit" "damage" "die" "die"}` after the keywords); without it the engine crashes when the damage
lands, with or without the DLL. The log warns when such an action is parsed.

```
{health_damage_pierce
    {effects "hit-scream" "hit-light" "hit-heavy" "die" "hit-explosion" "throw-off" "throw-off-and-die"}
    {explosive_treshold (* 70 %explosive_d)}
    {table ...}
    {multiplier 2.0}        ; optional: this unit takes 2x the damage
}
```

`{multiplier}`: `0` = immune, `0.5` = half, `2.0` = double; 0 to 65.535 (3 decimals); any position among the blocks.
It changes the damage and therefore which hit/die effect plays; `explosive_treshold` and the throw-off `table` use
the engine's separate "explosive amount" and are not affected. Without the DLL the engine reports the block as unknown,
so put it only in files that load through the launcher (the `40k_` folders).

### Events

| Script | Fired on |
|---|---|
| `{on "move_order" ...}` | every unit that receives a player move order |
| `{on "selection_of_units" ...}` | every owned unit the player selects |
| `{on "talk_<name>" ...}` | every other hard-coded talk moment: `talk_attack`, `talk_kill_enemy`, `talk_board_to_vehicle`, `talk_burning`, `talk_cover`, `talk_cannot_move`, ... (26 in all; spaces become underscores; the offline check prints the list) |

The names are quoted on purpose: `{on "name"}` is the engine's own custom-event syntax (what `{call "name"}` reaches),
so a script using them loads cleanly without the DLL; the handlers just never fire. Names can be changed in the ini.

`mod_side/scriptext_probe.inc` is a test block: paste it into a human interaction block, select a squad and move it
near enemies. With `verbose=1` the log shows the `event "..."` lines; the handlers also add the tags `sx_selected`,
`sx_move_order` and, under contact, `sx_move_under_contact` for 3 s, for any script or condition to check.

## Multiplayer

* **Simulation features** (`{health_damage_count}` extensions, `{multiplier}`): every player of a match must run the
  same DLL build, or the match desyncs.
* **Events** fire in network games too (`events_in_multiplayer=1`, the default since 0.8.2; 0 turns them off while a
  network game runs). Every player needs the same DLL build. `selection_of_units` is raised only on the PC of the player
  who selects, so in a network game its handlers must not change game state (tags, health, spawns...) or the match
  desyncs; keep them to local effects. `move_order` and the other talk moments come from the unit's simulation; that
  they fire on every player's PC is likely but not yet proven. To check: two machines with the DLL and `verbose=1`, one
  gives orders, and the other's log should show the same `event "move_order"` lines.
* `{if have_enemy}`, `{if health}`, `{if player}` and `{if team}` only read game state. Player numbers, battle teams and
  `{if player ally|neutral|enemy}` with an effector are the same on every PC, so they are safe in simulation scripts.
  `{if player ally|neutral|enemy}` **without** an effector compares with the local player, which differs per PC: local
  effects only.

## Settings and logs

All settings are in `gem_scriptext.ini` next to the DLL, grouped and commented: general (`enabled`, `log`, `console`,
`verbose`, `crash_log`, `require_mod`), script conditions, script actions and named events.

The log is `gem_scriptext.log` next to the DLL (with the launcher: `%LOCALAPPDATA%\Indomitus Launcher\payload\<hash>\`,
the newest folder is the latest run). At startup every feature prints `ON`, `off (ini)` or
`OFF (engine code not recognised)`. Useful lines:

| Line | Meaning |
|---|---|
| `event "move_order" -> entity ...` | (`verbose=1`) a named event fired (first 200) |
| `{multiplier 2.000} read for health_damage_blast` | (`verbose=1`) a `{multiplier}` block was parsed |
| `{if team}: player 3 is in multiplayer team "b"` | the battle team of a player, read the first time a script asks (first 24) |
| `{if team}: no multiplayer session - player 1 is in alliance team 2` | (`verbose=1`) outside a multiplayer battle (first 8) |
| `{if player enemy}: unit of player 1 vs the effector's player 0 -> enemy` | (`verbose=1`) what ally/neutral/enemy compared (first 12) |
| `EXCEPTION 0x... at ...` | (`crash_log=1`) fatal exception, registers and a stack scan |
| `ExitProcess(...) called from ...` | (`crash_log=1`) who ended the process; `exit trace ... done by another DLL` means a DLL loaded earlier (e.g. GEM Fauna) already writes this in its own log |
| `C++ exception thrown (...): a < b < c` | (`crash_log=1`) one line per distinct call chain (max 8). The engine throws and catches a few at every start: harmless unless a crash follows |

## Build and offline check

* **Build:** `build.bat`. Needs zig (portable `zig\` copied from the launcher project, zig on PATH, or
  `pip install ziglang`). Output: `dist\gem_scriptext.dll` (+ a copy of the ini).
* **Offline check** (Linux/WSL, gcc + clang): `sh tests/run_offline_check.sh <call_to_arms.exe> [editor] [server]`.
  It maps each exe and runs every finder (run this after a game update), tests the `{multiplier}` stub against fake
  parser frames (`tests/mult_stub_test.c`), and checks the Windows build (x86-64, LLP64) with the fake headers in
  `tests/stub` and the `build.bat` warnings as errors.

## Source layout

`src/gem_scriptext.c` is the only file the compiler sees; it includes the modules in `src/sx/` in dependency order
(a unity build, so `build.bat` and the tests need no file lists). Each module's runtime part is inside
`#ifndef SX_TEST`; its finders also build in the offline test.

| File | Contents |
|---|---|
| `sx/core.c` | logging, ini, PE sections, byte / string / RIP-reference search, RTTI vtables |
| `sx/engine.c` | shared engine bindings and their finders: Tag, interaction context, named events, TalkEvent, condition registry, script-flag condition, sensor, MP core, vitality, script reader, players / diplomacy, multiplayer teams |
| `sx/patch.c` | memory near the exe, thread suspension, 5-byte inline hooks, call-site redirection |
| `sx/health.c` | `{if health}`, `{health_damage_count}` extensions, vitality access |
| `sx/multiplier.c` | `{multiplier X}` |
| `sx/teams.c` | `{if player N|ally|neutral|enemy}`, `{if team N}` |
| `sx/conditions.c` | the added `{if}` conditions |
| `sx/modgate.c` | `require_mod`: named events fire only while a matching mod is enabled |
| `sx/events.c` | named events from `Entity::TalkEvent` |
| `sx/crashlog.c` | crash and process-exit logging |
| `sx/main.c` | dbghelp forwarders, the startup sequence, DllMain |
| `sx/test_main.c` | offline finder test (SX_TEST build only) |

## How it works

Verified on 1.067: game `6AB3DCC7`, editor `6AB3DD83`, server `6AB3DD21`.

* **`{if ...}` conditions:** the `{if}` parser walks a linked registry of `{name, ctor}` nodes, reached through an
  accessor right before the `unknown condition` error. ScriptExt prepends its nodes once the registry is built; no code
  is patched. Nodes and condition objects come from the engine's own allocator (the engine deletes them at exit).
* **`{if "have_enemy"}`:** the script-flag condition (`{set "x" 1}` / `{if "x"}`) gets a parse wrapper: if the
  interned name is `have_enemy`, the object switches to a clone vtable whose eval is the sensor check. Other flags are
  untouched.
* **have_enemy** reads what the talk condition `{seen enemy}` reads: entity -> sensor, four vectors of "seen" records,
  record flags (bit0 seen now, bit1 enemy).
* **Events:** `Entity::TalkEvent` is the one function all hard-coded talk moments pass through. A 5-byte hook fires the
  engine's `FireNamedEvent(ctx, Tag)` (the function behind `{on "name"}`) and then runs the original.
* **health:** the `eHealthDamageCount` vtable (parse, amount and factor slots) is hooked; the keyword hand-off between
  slots is thread-local.
* **player / team:** the owning player is a byte in the entity, found in `Entity::GetRelation`, which also leads to
  the diplomacy matrix (2 ally, 1 enemy, 0 neutral; 18 players). The battle's teams come from the session object the
  bot Lua API reads (`BotApi.Instance.team`): its player list holds each player's number and team Tag (`a`, `b`, ...),
  the same data the game uses to make team mates allies. That object belongs to the network layer, so it is read
  without risking a fault, and each player's team is cached until the player list changes. The editor has no
  multiplayer code, so there only alliances count. Ally / neutral / enemy repeat `GetRelation` (unit operatable bit,
  same player, matrix) with the reference from the script context: context[0] is the script's unit, context[1] the
  effector (valid when the flag the `{if effector}` condition tests is set); `{with_effector}` swaps the two. The local
  player comes from the getter next to `GetRelation` that `{if fogged}` also uses.
* **multiplier:** the block parser's "unknown block" error call is redirected to a stub that reads `{multiplier X}` for
  pierce/blast; the value lives in the object's unused padding word and the amount slot multiplies by it.

## Known limits and ideas

* `{set "have_enemy" 1}` has no effect on `{if "have_enemy"}` while the DLL is loaded; use another name for a plain flag.
* Named events nest at most 3 deep on one thread: an `{on "..."}` handler that raises talk events again (directly
  or through further handlers) stops firing ScriptExt events at that depth; the engine's own talk handling still runs.
* Unquoted `{on move_order}` is possible (hook the `{on}` parser) but unknown unquoted names make the engine drop the
  handler, so the quoted form is the safer contract.
* **Other launcher DLLs that read `eHealthDamageCount`'s vtable:** ScriptExt replaces its parse/amount/factor slots
  (5, 10, 12) for `max_health|set_health`. A DLL loaded after it that looks for engine code through those slots (GEM
  Teleport 0.1 finds `ReadFloat` through slot 5) sees ScriptExt's function and fails; it must read the original slot
  from the exe file or find the reader another way. ScriptExt logs the addresses it found (`script reader: Peek ... Float ...`).
* Ideas: `{if in_battle}` (same registry), class/side filters for have_enemy (`{if have_enemy "tank"}`).

## Changelog

* **0.9.1** `{if player ally}`, `{if player neutral}`, `{if player enemy}`: the engine's relation between the unit and
  the effector (inside `{with_effector}`: between the effector and the script's unit), or the local player when the
  event has no effector. Conditions now get the whole script context.
* **0.9.0** new conditions `{if player N}` (the unit's owning player, numbered as in the editor) and `{if team N}`
  (the battle team in multiplayer/bot battles, `a` = 1, `b` = 2; alliances elsewhere). The script reader functions are
  found on their own now (`script reader:` log line) instead of inside the health_damage_count finder. ini:
  `player_condition`, `team_condition`.

* **0.8.3** log cleanup from the 0.8.x logs: C++ exceptions one line per distinct call chain (the engine's own
  six at every start were 66 lines); the exit trace finds ExitProcess/TerminateProcess by name and says which DLL
  already traces them instead of "0 import(s) hooked"; the watch thread ends once conditions are registered and no
  `require_mod` gate is set. Review and cleanup: the real `dbghelp.dll` is loaded only when ScriptExt is installed as
  `dbghelp.dll` (injected by the launcher it no longer calls LoadLibrary inside DllMain); the leftover fauna sources
  (`src/sx/fauna.c`, `squads.c`, `ui.c`, `mod_side/fauna_selfheal.inc`) are dropped from the tree; the offline check
  type-checks the Windows build as LLP64 with warnings as errors (fake `stdint.h`); stale BreedTags tag-view hints
  removed from the probe and README.
* **0.8.2** named events fire in network games again by default (`events_in_multiplayer=1`).
* **0.8.1** review and cleanup after the fauna removal. Fixes: our `{if}` registrations no longer race the engine's
  own (wait until the registry is stable, compare-exchange insert, re-check for 2 s, never touch it at exit); the TalkEvent hook has its
  trampoline before it goes live; patching is skipped when the other threads cannot be stopped safely; the multiplier
  installs all-or-nothing; ini strings and mod names can no longer overrun their buffers; named events nest at most
  3 deep per thread (a handler that raises talk events cannot recurse without end). `require_mod` documented as what
  it does (it gates the named events). The offline check also finds the mod list.
* **0.8.0** all fauna features removed (vehicle panel health row, squad strip items and icons, fauna squad leader
  election, box-select and pace, `ui_debug`); they move to a separate DLL. The 0.7.0 source with them is kept in
  `_bckp/0.7.0_with_fauna/`. Script words, health, multiplier, events and the crash log are unchanged.
* **0.7.0** code reorganised into modules (`src/sx/`), no change to what the finders find. The 0.5.x fauna
  "panel leader" workaround and its ini key `fauna_squad_leader` are removed (replaced by real squad leaders in 0.6).
  UI hook traces only with `ui_debug=1`. The ini is regrouped and `crash_log` is listed.
* **0.6.3** fauna squad members take the leader's walk/run mode (`fauna_squad_pace`).
* **0.6.2** a fauna squad placed in the editor gets its leader at placement.
* **0.6.1** fauna squads get a real squad leader (`fauna_squad_elect`).
* **0.6.0** box-selecting a whole fauna squad selects its leader (`fauna_squad_select`).
* **0.5.2 - 0.5.10** "panel leader" workaround for fauna groups (answering the active-unit lookup for HUD code).
  Superseded by 0.6 and removed in 0.7.0.
* **0.5.1** review fixes: `have_enemy` stays off until its finder succeeds; thread-local health hand-off.
* **0.5.0** `{multiplier X}` in `{health_damage_pierce}` / `{health_damage_blast}`.
* **0.4.3** finders no longer fall back silently to 1.067 offsets; widget class checks.
* **0.4.0 - 0.4.2** `{if health}`, `{health_damage_count ... max_health|set_health [cap]}`.
* **0.3.x** fauna self-heal and `{if "sx_fauna"}` removed (use `{if prop "fauna_main"}`).
* **0.2.x** fauna HUD: vehicle panel health row, squad strip items and circle icons, mixed squads; crash logger;
  0.2.1 fixed a crash at exit (registry nodes now use the engine's allocator); 0.2.4 fixed a crash when selecting fauna.
* **0.1** `{if have_enemy}`, `{on "move_order"}`, `{on "selection_of_units"}`, `{on "talk_<name>"}`.
