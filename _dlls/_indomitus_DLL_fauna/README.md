# GEM Fauna 0.7.1

Engine extension DLL for Call to Arms: Gates of Hell (GEM, 1.067) that makes **fauna** - vehicle-class units whose def
carries `{props "fauna_main"}` - behave like infantry in the HUD and in squads. Split out of GEM ScriptExt 0.7.0 (the fauna
features were removed from ScriptExt in 0.8.0); the two DLLs patch different engine sites and load together.

Loaded by the Indomitus launcher (any file name). Same DLL for `call_to_arms.exe`, `call_to_arms_ed.exe` and
`call_to_arms_server.exe`; every address is found at startup from strings, RTTI and byte patterns, and a finder that
fails only turns its feature off (see `gem_fauna.log`).

## Features

| Feature | ini key | Scope |
|---|---|---|
| fauna = vehicle-class unit whose def has the fauna prop | `fauna_prop=fauna_main` | - |
| vehicle selection panel: health row (`pnl_health` / `bar_health` / `txt_health` from the mod's layout; shown for fauna only, other vehicles unchanged) | `fauna_ui`, `hp_bar_name`, `hp_text_name`, `hp_panel_name` | local (game, editor) |
| squad strip items for fauna (human item template, hp bar, circle icon from a def prop naming a file in `interface/scene/squad/`, else `fauna_default_icon`), mixed human + fauna squads | `fauna_ui`, `fauna_default_icon` | local |
| fauna squads get a leader (the engine elects none for 2+ vehicle-class members) | `fauna_squad_elect` | **simulation** - every MP peer needs the DLL |
| box-selecting a whole fauna squad, or loose fauna (the engine makes them a squad), selects only the leader (portrait, strip, direct control, squad orders) | `fauna_squad_select` | local |
| members copy the leader's walk/run (double-click) | `fauna_squad_pace` | simulation |
| members keep their formation slot (the engine lets vehicle-class followers idle until the leader is 15 m away) | `fauna_squad_formation`, `fauna_formation_radius_cm=150` | simulation |
| double-clicking a bandage in a fauna unit's inventory uses one and heals 15% of max health (nothing is used at full health) | `fauna_bandage`, `fauna_bandage_heal_percent=15` | simulation; multiplayer: the host uses it and sends the result |
| double-click run makes a fauna sprint (the "Fast" entry of its chassis), also out of a squad and in walk mode | `fauna_double_click_sprint` | simulation |
| the sprint request sticks on the fauna's foot chassis: double-click run, and **Shift in direct control** (stamina still limits it) | `fauna_sprint`, `fauna_sprint_debug` | simulation |
| direct-control HUD: health and stamina rows (human view icons) instead of fuel and speed for fauna | `fauna_dc_hud`, `dc_health_icon`, `dc_stamina_icon`, `dc_vehicle_icon_1/2` | local (game, editor) |
| over-head squad icon picked by a def prop: `{props "squad_view_<file>"}` (role icon), optional `{props "squad_view_leader_<file>"}` (leader icon); files in `interface/scene/squad_view/` | `fauna_head_icon` | local (game, editor) |
| stamina bar in the vehicle panel (`bar_stamina` / `txt_stamina` / `pnl_stamina`) for fauna with a vitality `{Stamina}` block | `fauna_stamina_bar`, `stamina_bar_name`, `stamina_text_name`, `stamina_panel_name` | local |
| `{extender "vitality"}` (Health) on a wheel/track/human_carrier chassis no longer kills the game (any unit, not only fauna) | `vitality_any_chassis` | **simulation** |
| diagnostics | `crash_log`, `ui_debug`, `fauna_sprint_debug` | - |

Mod side: the vehicle panel layout (`GameVehiclePanelView.xml`) needs a progress bar `bar_health` (copy it from the
human panel), optionally a text `txt_health` and a container `pnl_health`. Fauna defs carry `{props "fauna_main" ...}`;
a further prop named like a file in `interface/scene/squad/` picks the strip icon.

## Build

`build.bat` - zig cc, mingw-w64 target, unity build of `src/gem_fauna.c`. Uses `.\zig\zig.exe`, else
`..\_indomitus_DLL_scriptext\zig\zig.exe`, else zig on PATH / `python -m ziglang`. Output `dist\gem_fauna.dll` +
`gem_fauna.ini`; copy both into the launcher payload and rebuild the launcher.

Offline check (Linux/WSL, after a game update): `sh tests/run_offline_check.sh <game.exe> <editor.exe> <server.exe>`
maps each exe and runs every finder (`FX_TEST` build), then syntax-checks the Windows build with the stub headers.

## Source layout

```
src/gem_fauna.c     unity file: version, ini/log names, include order
src/fx/core.c       log, ini, PE sections, .pdata lookup, byte/string/RIP search, RTTI vtables
src/fx/engine.c     Tag ctor (static Tag "bandage"), condition tables, def props ({if prop}), vitality (health_level)
src/fx/patch.c      alloc_near, patch_code (all other threads stopped, else no patch), install_jmp5, redirect_call, vt_hook
src/fx/fauna.c      is_fauna, entity_vitality
src/fx/ui.c         find_ui / install_ui: PanelVehicle::Update, PanelSquad GetIds/CreateItem/GetProperty/Update,
                    ePanelHuman refresh/tick, CreateItem "panel_human" entry stub, icon folder scan
src/fx/sprint.c     find_fauna_sprint / install_fauna_sprint: eChassisFoot SetMovementMode / SetMovementSpeed for fauna,
                    brain movement mode set by the move order (fauna_sprint_order)
src/fx/dcview.c     find_dcview / install_dcview: GameControl::eVehicleView (direct-control HUD) rows for fauna
src/fx/squads.c     find_squad_select/elect/pace, find_box_create + hooks: fx_box_isvehicle, fx_box_cmd_ctor,
                    fx_elect_leader (ElectLeader entry), fx_squad_update, fx_editor_set_leader, fx_move_change / fx_move_give
src/fx/follow.c     find_follow / install_follow: the vehicle follow handler's and root state's GetLeader calls answer
                    a slot record for fauna (asm stub at the handler site also sets R in xmm6)
src/fx/bandage.c    find_bandage / install_bandage: UseItem operation step (vtable slot 9) for fauna + bandage ->
                    RemoveMany one + HealPercent; inventory window click logged for fauna (diagnostics)
src/fx/crashlog.c   vectored exception log, ExitProcess/TerminateProcess IAT trace
src/fx/sqview.c     find_sqview / install_sqview: GameControl::eSquadView builder (extra squad_view icons) and icon classifier calls
src/fx/main.c       init order: tag ctor + prop cond -> squads (sim: elect, formation) -> pace + sprint -> vitality -> bandages -> UI
                    (+ stamina fields, head icon) -> box select (+ box create)
src/fx/test_main.c  FX_TEST main
```

## Formation fix (0.2.0) - how it works

The squad update writes each member's formation slot (world x,y) to `brain+0x138` and raises the follow request
(brain flags bit 9 / bit 10). Infantry walk to the slot; vehicle-class units run the vehicle follow handler
(editor 0x14081B8D0), which asks `GetLeader(brain)` and idles while within scale x 15.0 = 15 m of the *leader*, else
drives to the leader with stop radius 0.9 R. GEM Fauna redirects the two `GetLeader` calls of that code (the handler's,
exe+0x9926A0 / 0x81B949 / 0x7A7760, and the vehicle root state's re-target check, exe+0x99030A / 0x8195E2 / 0x7A53CA):
for a fauna member of a squad without a real vehicle they return a thread-local record positioned at the unit's own slot
(own z), and the handler-site stub sets xmm6 (= R) to scale x `fauna_formation_radius_cm`. All found by patterns:
GetLeader from the squad/leader offsets, the sites from `movss xmm6,[scale]; mulss xmm6,[15.0]; call GetLeader` and
`cmp byte [rsi+3B9],0; je; mov rcx,[rsi+30]; call GetLeader`, the slot offset from `mov rax,[r+0xDC8] .. movsd [rax+SLOT]`.

## Leader election - how it works

`ElectLeader` is hooked **at its entry** (5-byte jmp, relocated prologue), so every caller goes through `fx_elect_leader`:
squad creation, the squad update's re-election, the map/placement loader's "no leader read -> elect", the create-squad
command (built by the box select for loose units), and anything else. The engine's answer is always taken first; only when
it finds nobody and the list's vehicle-class members are all fauna (2+) does the fauna rule apply: the first member passing
the engine's own candidate test (ElectLeader on that member alone), infantry first, in list order; if nobody passes (new
units are not "operatable" - `entity+0xDB4` bit 8 - until the engine activates them), the first in-scene member - only
when the members' squad has no leader (never replaces one: a SetLeader resets the squad anchor and fires an event on every
member). If the entry hook cannot be installed, every call site is redirected instead. The squad update raises the
engine's own "elect again" flag for a leaderless fauna squad.

## Leader-only selection - how it works

- **Box over a whole existing squad:** the box select picks the leader only for a human leader; for a vehicle-class
  leader it adds a chassis check fauna fail. `fx_box_isvehicle` answers "not a vehicle" for a fauna leader at that one call.
- **Box over loose units:** the box select adds every unit, then builds the engine's create-squad command (whose
  constructor elects the leader) and nothing trims the selection. `fx_box_cmd_ctor` wraps that constructor call: if the
  elected leader is fauna, every other boxed unit is deselected (`SetSelected(e, 0)` plus the engine's selection
  add/remove). Local UI only; the command is untouched.
- **A squad's first leader while it is selected** (editor squad placement): `fx_elect_leader` leaves only the leader
  selected among the members flagged selected (local selection state).

`patch.c` is the ScriptExt 0.8.1 patcher: `patch_code` stops every other thread outside the bytes being written or does
not patch; hooks publish the original/trampoline pointer before the jump is live.

## Bandages - how it works

Double-clicking an item in the inventory window (`GameControl::eInventoryBoxView`, click handler = vtable slot 39; it
needs the owner alive and operatable, `entity+0xDB4` bit 8) queues the engine's `UseItem` inventory operation. Its process
step (`Inventory::eOperation_UseItem` vtable slot 9) runs once with state 1: a human with a `bandage` item gets the
self-bandage order, a vehicle-class owner only the "load into a vehicle holder" path - so for fauna nothing happened.
`fx_useitem_tick` wraps slot 9: owner fauna + item class tags holding the engine's static Tag `bandage` -> one bandage is
removed (`RemoveMany` on the operation's item view, as the console `inventory remove` does; reserved items or items in a
holder are left alone) and the unit gets `HealPercent(entity, fauna_bandage_heal_percent)` (the engine's call used by the
medic "care" action: capped at max, fires the health-changed event). At full health nothing is used. Anything else goes
to the engine.

Multiplayer: a client's double-click reaches the host (or dedicated server) as an interface action; only the host runs
the operation (the engine's own "is client" test, taken from its inventory-remove code, keeps clients out). On the host
the engine's step runs first (its operation-state sync to the clients), then the bandage is used as in single player and
the result goes out with the engine's own commands: `eInventoryRemoveCommand` {fauna, the bandage's class tags, count}
(vanilla execute: every client removes the same item - the engine sends it the same way when an action takes an item out
of an inventory) and `eUseMedkitCommand` {fauna, new health} (the medic heal replication; its execute only accepts human
targets, so GEM Fauna wraps it on every peer and applies the sent health to a fauna target). Every player needs GEM Fauna
(as for the squad features). If those engine pieces are not recognised, fauna bandages stay off in a network session.
The first double-clicks on a fauna inventory are logged with the
operatable/active flags, so a click the engine drops before it becomes an operation shows up in `gem_fauna.log`.

A Ctrl+number group is not a squad: the engine has no leader for it (same for infantry).

Details, addresses and patch sites: project doc `gem-fauna-dll-handoff.md`.

## Stamina (mod side) - how it works

The engine's stamina lives in `Actor::eVitalityExtender` - the same health component fauna already have - and its timer
update runs for fauna too: it drains or refills stamina by the foot chassis' movement mode and caps the chassis' movement
mode through `eChassisFoot::SetStaminaMovementModeLimit` (replicated to clients by `eSetStaminaMovementModeLimitCommand`,
which accepts any actor). Fauna had none only because their vitality block has no `{Stamina}`. Add one to the fauna
breed's `{extender "vitality" ...}`, e.g. (all four load columns equal: the load column comes from a human field):

```
{Stamina
	{max		100}
	{current	100}
	{inc		;// movement_mode  load[0..3], per second
		{stay		+3	+3	+3	+3}
		{walk		+3	+3	+3	+3}
		{slow		+2	+2	+2	+2}
		{normal		+1	+1	+1	+1}
		{fast		-6	-6	-6	-6}
	}
	{load				0	40	65	100}
	{movement_limits			;// sprint stops at 1 stamina, comes back at 20
		{Fast		1		20}
		{Normal		0		0 }
		{Slow		-1		-1}
	}
}
```

Copy `bar_stamina` (and optionally `txt_stamina`, a container `pnl_stamina`) from the human panel into the vehicle panel
layout to see it; GEM Fauna fills them for fauna with a stamina block and hides them otherwise.

## Double-click sprint - how it works

The move action picks the walk/run mode from `MODE_TABLE[run/walk + chassis kind + 2 * (unit in running mode)]`; in walk
mode (brain+0x398 bit 14) a double-click asks for the double-flick walk speed, which for a fauna Run block reads as no
run. At the pace hook (the movement-change GiveOrder of the move action), a fauna that got a run request (first argument
0; -1 = walk) gets a second movement change with `MODE_TABLE[3]` (run in running mode = the chassis "Fast" entry); squad
members copy it. A stamina limit still turns Fast into Normal.

## Sprint for fauna (0.6.1-0.6.2) - how it works

Fauna run on `Actor::eChassisFoot`, the human foot chassis. Its speed comes from the chassis movement mode (0 slow,
1 normal, 2 fast = sprint, 3 default, 7). Two virtual calls request it: `SetMovementMode(mode)` (slot 44) and
`SetMovementSpeed(speed)` (slot 43). A human sprints through `Order::eMovementChange` -> `SetMovementMode(2)` (double-
click) or `eChassisFoot::ManualSimulation` (direct control: Shift = chassis key bit 6 -> `SetMovementMode(2)`).

Two things kept a fauna at normal speed:
1. **The request never applied.** A request is only stored as pending; it is applied by the chassis *commit* (the
   function that also sends `eChassisFootSetModeCommand`) when the unit's FSM reaches its "movement mode" state. A
   fauna's FSM never does. GEM Fauna runs the commit right after a fauna's request whenever the resolved mode or speed
   differs from the applied one. The commit keeps the engine's own checks: IsClient, movement type, the stamina limit
   (`table[limit][mode]`, so a tired fauna drops to normal; a def without `{Stamina}` is never limited), multiplayer.
2. **The vehicle brain asked for normal again.** A fauna has a vehicle brain whose states re-request the brain's
   movement mode (brain+0x398 bits 6..9, 1 for vehicles) when a move starts. The move order (pace hook) now writes those
   bits: 2 for a double-click run, back to 1 on any other order (squad members too), and for a fauna that should sprint
   a `SetMovementMode(1/3)` becomes `SetMovementMode(2)` ("should sprint": in direct control, Shift held while moving -
   ManualSimulation's own key test; otherwise brain mode 2).

After a sprint order the fauna keeps the sprint pace for its own moves until the next player move order.
`fauna_sprint_debug=1` logs the first chassis calls and commits (caller, direct-control flag, keys, brain mode, result).

## Direct-control HUD (0.6.1) - how it works

In direct control a vehicle-class unit gets `GameControl::eVehicleView` (`vehicle_view` in SceneGameManualView), built in
code: two rows {panel, picture, text, loc key} at view+0x190 and view+0x1E0 - fuel (from a fuel chassis; a foot chassis
has none, so the text stayed empty) and speed ("14 km/h (Ground)"). vt slot 2 (event 1 = the view's timer) runs Update;
GEM Fauna runs after it: for a fauna, row 1 gets the health icon (`/interface/scene/human_view/health`) and "cur / max",
row 2 the stamina icon and "cur / max" (row hidden when the def has no Stamina block). Any other vehicle gets the fuel and
speed icons back. No layout change is needed (the view has no XML).

## Over-head icon - how it works

`GameControl::eSquadView` builds one sprite per Tag of a static list (normal, medic, mg, at, sniper, engineer,
squad_leader) from `/interface/scene/squad_view/<name>` into a map, and per unit a classifier picks the Tag from the unit's
role flags (leader: squad_leader). GEM Fauna appends every other file of `interface/scene/squad_view/` (file repository)
to that list before the first view is built, and wraps the two classifier calls: for a fauna with
`squad_view_<file>`, the six role entries point at that sprite while the engine classifies, then are restored
(`squad_view_leader_<file>` does the same for squad_leader).

## Not done: crushing obstacles in direct control

Fauna use `{Chassis "foot"}`. Under AI movement, crushing comes from the vehicle state machine's "crush" state on the
path; in direct control a tank crushes through its track chassis physics, which a foot chassis does not have. Doing it
for fauna means driving the obstacle-crush code by hand and needs a runtime trace first (next step: a diagnostic build).

## Vitality on any chassis (0.7.0) - how it works

The engine ties the vitality extender to the foot chassis: its timer update (health and stamina regeneration, host side)
ends every tick with `dynamic_cast<eChassisFoot&>(entity->chassis)` - a *reference* cast, which throws `std::bad_cast`
for any other chassis - followed by `eChassisFoot::SetStaminaMovementModeLimit(limit)`. It gets there even without a
`{Stamina}` block (the empty movement_limits case still applies limit 2), so a unit with `{extender "vitality" {Health}}`
and `{Chassis "human_carrier"}` (the Carnifex), wheel or track was a fatal error on its first tick. GEM Fauna flips the
cast's throw flag (the `isReference` argument, 1 -> 0) and redirects the SetStaminaMovementModeLimit call to a wrapper
that skips a NULL chassis. The client-side path of the same function calls a base-class virtual every chassis has.
Stamina limits still work on foot chassis; a non-foot chassis simply has no stamina speed limit. Finder: the
SetStaminaMovementModeLimit call inside the timer (the function longer than 0x200 bytes), preceded by
`mov rcx,rax ; xor r8d,r8d ; mov edx,ebx` and the `mov dword [rsp+0x20],1` within 0x50 bytes before it.

## Performance notes (0.6.3)

Nothing runs per frame unless a fauna is selected or in direct control. Then: the vehicle panel rows (widgets resolved
once per selection, bar redrawn and caption written only on change), the direct-control rows (text only on change,
icons only on a unit change), and the chassis speed wrappers (one cached per-def lookup for non-fauna calls). `is_fauna`
caches its answer per def (64-entry direct-mapped table). Orders, election, selection, bandages and the over-head icon
run on player actions only. All debug logs are capped and off by default.

## History
- 0.7.1 - fix: the stamina bar (vehicle panel and direct-control HUD) was gone in 0.7.0 - the stamina fields are found
  through the call the vitality patch redirects, so they are now looked up before the patch.
- 0.7.0 - vitality on any chassis: the vitality timer's foot-chassis reference cast no longer throws, so `{extender
  "vitality" {Health}}` works with human_carrier/wheel/track chassis (Carnifex). Simulation; every MP peer needs it.
- 0.6.4 - crash log: C++ throws are logged with the thrown type and message (engine errors), up to 40, with a
  16-frame chain. Diagnostics only.
- 0.6.3 - cleanup: per-def fauna cache, vehicle-panel widgets resolved once per selection, captions and direct-control
  texts written only on change, sprint debug log off by default. Behaviour unchanged from 0.6.2.
- 0.6.2 - fauna chassis mode/speed changes are committed right away (their FSM never commits them), so the sprint
  request from direct control and from the double-click actually applies; debug log adds the commit lines.
- 0.6.1 - sprint fix: the fauna foot chassis keeps the sprint request (double-click run, Shift in direct control) instead
  of the vehicle brain's normal speed; sprint debug log. Direct-control HUD shows health and stamina for fauna instead of
  fuel and speed. Patterns pass offline on game, editor, server.
- 0.6.0 - double-click sprint for any fauna (also alone / in walk mode); over-head icon from a def prop
  (`squad_view_<file>`, `squad_view_leader_<file>`; extra squad_view files loaded); stamina bar row in the vehicle panel
  (stamina itself is a vitality `{Stamina}` block in the def - see above). Patterns pass offline on game, editor, server.

- 0.5.1 - fauna bandages in multiplayer: the host uses the bandage and sends the inventory change and the new health
  with the engine's `eInventoryRemoveCommand` / `eUseMedkitCommand` (the latter wrapped to accept fauna targets).
- 0.5.0 - fauna bandages: double-clicking a bandage in a fauna unit's inventory uses one and heals
  `fauna_bandage_heal_percent` (15) % of max health (`fx/bandage.c`); patterns pass offline on game, editor and server.
- 0.4.0 - cleanup after the working 0.3.4 (behaviour unchanged): removed the two hooks the log showed never firing
  (0.3.1 SetSelected wrapper at the select-leader call sites, 0.2.1 SquadOf wrapper - the one hook that set a leader from
  a local mouse action); box create deselects through SetSelected as well, so a first box no longer deselects twice;
  SetSelected is optional (its absence no longer turns leader election off); one member-vector offset (checked against
  the box select's); pace reaches up to 64 members (was 16); clearer election log labels; the 0.3.2 "not fauna"
  diagnostic only with `ui_debug=1`.
- 0.3.4 - first box over loose fauna selects the new squad's leader (wrapper of the create-squad command constructor in
  the box select). Confirmed working in the editor 2026-10-29.
- 0.3.3 - a squad's first fauna leader, if selected, leaves only itself selected (inside fx_elect_leader).
- 0.3.2 - diagnostic: why a vehicle in the panel is not fauna (props listing).
- 0.3.1 - leader-only selection right when a fauna squad is created from a box selection (SetSelected wrapper at the
  two select-leader call sites, found by pattern in all three exes).
- 0.3.0 - merged onto the 0.2.3 cleanup tree (engine.c, fauna.c, ui.c, tests, ini as cleaned up; squads.c before
  ui.c in the unity file since ui.c owns squad_debug). ElectLeader hooked at its entry (all callers); forced first-member leader for a leaderless fauna squad whose
  members are not yet operatable, never during re-validation; ScriptExt 0.8.1 patch.c; IsVehicle redirect through
  redirect_call.
- 0.2.2 - every ElectLeader call site is hooked (3 per exe): the third one is the squad_create command (editor unit
  tool / in-game create-squad on a selection), which elected the new squad's leader itself and got NULL for fauna.
- 0.2.1 - leaderless fauna squad gets its leader at the first box-select (SquadOf wrapper); log line when the engine
  election finds nobody eligible.
- 0.2.0 - fauna followers keep their formation slot (`fx/follow.c`); patterns pass offline on game 6AB3DCC7,
  editor 6AB3DD83, server 6AB3DD21.
- 0.1.0 - first standalone build: ScriptExt 0.7.0 fauna/ui/squads modules unchanged, new init and ini
  (`[fauna]` section, `gem_fauna.ini` / `gem_fauna.log`), no dbghelp proxy, no script/event code.
