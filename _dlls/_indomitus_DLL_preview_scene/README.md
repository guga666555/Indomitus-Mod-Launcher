# GEM PreviewScene 0.2.0 (test build)

Goal: the conquest unit preview shows **a map + mission you build in the editor**, and the **unit you select in
conquest** is spawned there and moves: soldiers walk, vehicles drive **with their crew inside**, planes fly.

The plan copies how **Men of War II** does its menu background (studied 2026-10-31): its background is an ordinary
map with an ordinary mission (`village/0.mi`). The menu only raises the event `metagame_unit_changed1`; the
mission's own triggers delete the previous unit, spawn the selected one (`mp_spawn {selected_metagame_unit 1}`),
move it along waypoints and switch camera views. Gates of Hell has the same mission trigger system, so almost all
of it can be built in the GoH editor. The DLL adds what GoH lacks.

**0.2.0 is a test build.** Unless you fill in the ini it changes nothing. It answers one question: does a mission
loaded into the preview scene actually run (triggers fire, units move)?

## What 0.2.0 does
1. `preview_map=` (ini): the unit preview's scene (`library`) loads this map/mission instead of nothing. The
   descriptor is rebuilt with the engine's own parser, so it accepts what the game itself uses: `map/path` or
   `map/path:mission`.
2. `unit_changed_event=` (ini): when the unit shown in a preview changes, this mission event is set.
3. Logs, always:
   * every scene load (name, map, mission, map folder, load time; game and editor);
   * the preview's entities and the unit template it shows (when it changes);
   * every mission command that runs (event, trigger, spawn, squad_create, reinforcement, waypoint,
     squad_to_waypoint, actor_to_waypoint, board, delete, entity_state, air_state, scenario; game and editor),
     with the event/trigger name. If these appear while you're in conquest, the preview's mission is running.

## Build and install
1. `build.bat`
2. Copy `dist\gem_previewscene.dll` + `gem_previewscene.ini` into `..\..\launcher\payload\` and rebuild the launcher.

## The test
**A. A tiny test mission (editor).** Any small map you have is fine. In its mission:
* Helpers / events: declare an event `preview_unit_changed`.
* Trigger `start`: on mission start, action `event` `preview_started` (set). Also place one unit with a move order
  or a waypoint, so something moves.
* Trigger `unit_changed`: condition = event `preview_unit_changed`; actions = event `preview_unit_changed` reset,
  then something you can see (spawn a squad, an effect, ...). Let it re-arm itself (like Men of War II's
  `spawn/1` trigger, which calls `trigger` on its own name at the end).
* Test-play it once in the editor (started through the launcher): `gem_previewscene_editor.log` then shows how
  the editor names the map and mission, and the mission commands it runs.

**B. Point the preview at it.** In the payload `gem_previewscene.ini`:
```
preview_map=<what the editor log shows as map>[:<mission>]
unit_changed_event=preview_unit_changed
```

**C. Game.** Open conquest, preview a soldier squad, a tank (to see how the crew is listed) and a plane, open
the research tree preview too. Close the game.

**D.** Send `gem_previewscene.log` and `gem_previewscene_editor.log` (payload folder,
`%LocalAppData%\Indomitus Launcher\payload\<fingerprint>\`).

To undo: empty `preview_map` and `unit_changed_event`.

## What comes after the test (if the mission runs)
* **Spawn the selected unit from the mission** (GoH's version of `selected_metagame_unit`): the DLL tells the
  mission's spawn action which unit conquest selected. It is spawned the way battles spawn reinforcements, so a
  vehicle comes with its crew **inside** - not standing next to it as in today's preview - and can drive.
* Hide the preview's own display units (the ones that show the crew outside) while the mission unit is shown.
* Camera: use the mission's camera views (like Men of War II's `startview` / `specialview_*`).

## ini (`[previewscene]`)
| key | default | meaning |
|---|---|---|
| `enabled` | 1 | 0 = the DLL does nothing |
| `log` / `console` | 1 / 0 | log file / console window |
| `preview_map` | empty | map[:mission] for the preview scene; empty = vanilla |
| `unit_changed_event` | empty | mission event set when the previewed unit changes |
| `log_scene_loads` | 1 | log every scene load |
| `log_preview_units` | 1 | log the previews' entities and shown template (game only) |
| `log_mission_commands` | 1 | log mission commands that run |
| `preview_check_ms` | 1000 | how often previews are checked for changes |
| `max_dumps` | 60 | cap on preview dumps per run |
| `mission_log_max` | 40 | per command type, then every 100th |

## How it works (game 1.067, PE timestamp 6AB3DCC7; editor 6AB3DD83)
* Scene::eManager vt[5] `Load(scene, desc, flag)`: desc = map / mission / map folder strings at +0x00/+0x28/+0x50;
  scene name at +0x860. The unit preview loader (0x140363100) creates scene `library` with an empty descriptor.
* `MapDesc 0x140A2BDE0(desc, std::string)`: splits `map:mission` on ':' (or parses a path) and builds the folder
  `/map/<map>/`. 0.2.0 calls it again on the preview's descriptor with `preview_map`.
* Mission commands `Helper::eCmd_*`: vt[7] Execute(cmd, ctx). `eCmd_event::Execute` (0x1409C3780) shows the mission
  event table (0x141485658), `EventFind` (0x1409D55E0) and `EventSet(table, Tag*, on, delay)` (0x1409D5730); event
  names are Tags (text at +0x40).
* eGUI_MultiplayerLibrary: vt[1] OnEvent (0x101D scene ready, 0xFD2 tick), render engine +0x280, shown template
  +0x310 (zeroed by Clear, vt[74]).
* eLibraryRenderEngine::Quant (0x140363300) only ticks its own display entities (no AI/physics) - the reason the
  moving units have to come from a mission.
* Every address/offset is found at startup from RTTI and byte patterns; only vtable slots are swapped; logging
  reads are checked with VirtualQuery.

## Offline check
`tests/run_offline_check.sh <call_to_arms.exe> <call_to_arms_ed.exe>` (Linux/WSL, gcc + clang). Current result:
game OK (scene, map parser, mission events, 13/13 commands, preview); editor OK (scene, 13/13 commands; it has no
unit preview).
