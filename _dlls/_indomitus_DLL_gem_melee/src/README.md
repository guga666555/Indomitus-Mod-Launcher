# GEM Melee 0.1.20

`gem_melee.dll` is GEM MeleeStance 0.7.42, GEM MeleeChase 0.5.4 and GEM PenFilter 0.1.1 in one engine hook DLL for Call to
Arms: Gates of Hell 1.069 (the Indomitus launcher payload; game, editor and dedicated server, one file for all three exes).
It replaces `gem_meleestance.dll`, `gem_meleechase.dll` and `gem_penfilter.dll` and their three inis.

0.1.1 (Nov 21 2026, after Alex's first editor sessions on 0.1.0): the melee toggle's switch-off click gives the gun back
(section 6 item 8; 0.7.42 and 0.1.0 left the melee weapon in hand until the next fight); `AutoMeleeTag=melee_infantry,melee_auto`
(`melee_auto` = may melee without the `[Zerg]` charge, on the 12 assault marine breeds); the charge run is re-aimed after at most 10 m of enemy movement (item 9,
`[Charge] MoveRefreshMaxUnits=200`). Everything else is 0.1.0.

0.1.2 (Nov 21 2026, Alex after playing 0.1.1: "Yes, build it"): intercept lead for the far charge run (section 6 item 10,
`[Charge] Lead=1`): the run goes to where the enemy will be. Everything else is 0.1.1.

0.1.3 (Nov 21 2026, Alex on 0.1.2: "he only updates his pos once he gets to the original location"): a running charge run is
re-aimed inside its own order, the engine's squad-follow way, so the goal follows the enemy without a stop (section 6 item 11,
`[Charge] Smooth=1`); a new order, where that is not possible, is given when the run's lead time is used up.

0.1.4 (Nov 21 2026, Alex on 0.1.3: "works really well" but the frames dropped during the charge): the in-place re-aim runs inside
the engine's quant (from a weapon-pick scan), not from the dll's tick before it; the editor asserted once per re-aim (item 11).
The log gets a `[core] mem:` line (private bytes, working set) when the process's memory has moved 64 MB, or once a minute.

0.1.5 (Nov 22 2026, Alex on 0.1.4: "Frames were fine"; his log had 14 new run orders at the enemy a marine was already running at,
with no reason given): log only. Such an order gets a line `  ^ new order, not in place: ...` naming the test that said no, a
refused merge gets its own line, and an `hb: re-aims` line carries the counts. No decision changes.

0.1.6 (Nov 22 2026, Alex on the 0.1.5 log: "Yes, build it"): every order the dll gave was appended to the unit's order queue
(the engine's AddOrder flag 1), so after a target switch the new run only started when the old one had arrived, and a melee order
waited for the run to end. A new charge order now takes the place of the dll's own run order in front (flags 5; item 11,
`[Charge] ReplaceOwn=1`). A player's or the bot's order in front is never cancelled.

0.1.7 (Nov 22 2026, Alex on 0.1.6: "dudes are bugging out running off the map", "cant they just receive an updated attack order
every 2s once they choose a target"): one charge order at a time, nothing is appended any more (his log had 33 orders queued on
one unit). The order takes the place of the dll's own run order or of an attack order in front; behind any other order nothing is
given until it has ended; switching the stance off cancels what the dll left queued (item 11).

0.1.8 (Nov 22 2026, Alex on 0.1.7: "that was bad. 1.6 was way better, we just need to fix those dangling orders"): the 0.1.6 rules
again (0.1.7 gave nothing while another order was in front, and the engine's own eComplexOrder is in front of a fighting unit
nearly all the time: 150 orders not given in 16 s), with two limits: behind an order that is not the dll's own run order only
ONE order of the dll's waits, and switching the stance off cancels the unit's queue while an order of the dll's is in it.

0.1.9 (Nov 22 2026, Alex: "add in gun volumes before tracks"): gun, barrel and breech volumes carry `no_target` (vanilla
`main_weapon` define), so the engine's volume picker never collects them. `[PenFilter] AimPriority` takes `+tag` now: a part
written that way is offered by the dll itself (hook P puts the pierceable ones in place of the picker's list on a row without
`{volume "x"}`, when no group before theirs has a reachable pierceable part; hook A counts them for the ammo verdict; one the
aim solver cannot reach is blocked like an armour miss). Default: `+gun,+gun1..+gun7` as a group of its own before the tracks.

0.1.10 (Nov 22 2026, Alex: four dreadnoughts walked up to a Baneblade and did not swing; ogryns punched a dreadnought only once it
clipped into them): melee reach of vehicle-brain units against wide targets. The engine measures a melee weapon's range to the
target's centre; humans have had the hull-contact rule since MeleeStance 0.7.3, walkers and fauna had nothing. Now the pen module
hands out the nearest volume centre of a target (`core_hull_point`); stance gives it to the engine's range test for a vehicle
brain's melee ammo against a vehicle (`veh_shift`), pen cuts that weapon's volume pick to the 3 nearest parts, and chase measures
reach / stop / hold / priority to the hull (`tgt_dist`) and holds a melee unit that is in reach of it.

0.1.11 (Nov 22 2026, Alex: "Yes, cut the cost"): the weapon-pick hook cost 4.3-7.8 us per pick in his 6v6 fights (the engine's
own match: 0.28 us), nearly all of it walks over entity tag vectors repeated for every candidate ammo of every enemy. `vec_has`
takes the list words' lengths once per call and, on the simulation thread, keeps its answer per (vector, begin / end, list, sim
clock value): one scan per entity, list and quant. No setting; the answer is the scan's, a same-size tag swap is seen one quant late.

0.1.12 (Nov 22 2026, Alex on 0.1.11: the dreadnoughts "still walked up to it and stood in front not meleeing"): his log shows the
melee weapon picked and aimed at the hull, and a new approach order twice a second for a dreadnought 8.1 m from the Baneblade's
centre: its guns are useless there, so the engine's own approach range is the melee reach, to the centre. The hold at the hull
now covers every approach to melee reach (0.1.10: melee-priority / melee-only units), threshold = the reach. New `[pen] M` line:
what vehicle-brain melee picks end in (solver statuses, picks that never reach the solver).

0.1.13 (Nov 22 2026, Alex on 0.1.12: "dreads still stand in front not meleeing. is it ammo.set file making their other wapons take
priority?"): yes in effect. The engine chooses one weapon per target the ammo.set way and keeps it while it works: the
dreadnought's heavy flamer from 40 m on, also at the hull (the fist's picks stop at exactly 40 m in his log). The dreadnoughts are
`ai_prioritize_ranged`, which MeleeChase's hold-ranged-fire-in-melee-reach rule skipped. Now a ranged-priority unit in melee reach
of a vehicle-class target holds its ranged weapons too, so the engine falls through to the melee weapon. New `[chase] eval:` lines.

0.1.14 (Nov 22 2026, Alex on 0.1.13 with a screenshot of three dreadnoughts at the hull: "still standing in front of the baneblade
not using melee"): the engine's weapon selection drops a weapon whose reach test against the target's centre fails before
ammo.set is asked, and the target evaluation tests the selected weapon's range against the centre again. Two call-site hooks in
the chase module (`sel_wrap`, `evr_wrap`) answer both for a melee weapon of a vehicle brain with a part of a vehicle-class target
in range. The selection hook is found on the editor exe only so far.

0.1.15 (Nov 22 2026, Alex: the claw works against the Baneblade under direct control; "cant we just let the ai shoot multiple
weapons"): the target evaluation hands the target to one weapon. In the SetTarget hook, when that weapon is a ranged one and a
part of a vehicle-class target is inside a melee weapon's range, the melee weapon is handed the same target in the same call.
Works on all three exes (the 0.1.14 selection hooks: editor only). Ranged weapons firing together is not built.

0.1.16 (Nov 22 2026, Alex on 0.1.15: "they still stand there"; "an attack order against LR it works"): the fist had its target
and an accepted aim, but the engine's fire task and attack task compare `range(weapon, target)` with the distance to the target's
centre. All 6 call sites of that function (found on all three exes) go through `rwt_wrap`: a melee weapon of a vehicle brain with
a part of a vehicle-class target inside its range is in range of that target.

0.1.17 (Nov 22 2026, Alex on 0.1.16: same at the Baneblade, a Macharius is not hit either, a Leman Russ is): range was never the
gate (the 0.1.16 rule fired 0 times). The engine fires a weapon only when its aimer's status is 3 / 4 (aim bones on the picked
part inside their limits) and the aimer's fire line check is clear. `ast_wrap` / `blk_wrap` on those two slots of the five aimer
vtables (all three exes) answer status 3 and a clear line for a melee weapon of a vehicle-brain unit with a part of its entity
target inside the weapon's range, when the engine's readiness gate says the weapon is ready. Log lines name what refused.

0.1.18 (Nov 22 2026, Alex on 0.1.17: "ogryns cant melee tanks, dreads dont do it"; direct control works): with status 3 and a
clear fire line the engine did call the shooter, whose update only shoots when the 2D distance from the aimer to a point is
inside the ammo's min / max range: the cursor's aim point under direct control, the target entity's CENTRE for the AI. `spt_wrap`
on the +0x60 slot of the three shooter vtables (all three exes) answers true when that test fails for a melee weapon of a
vehicle-brain unit, the point is the target's centre and a part of the target is inside the weapon's range.

0.1.19 (Nov 22 2026, Alex: "both"): the 0.1.14 weapon-selection reach test is found on the game and server exes too (one site
each); entity tag `melee_hold` (the vehicle melee button's off side) = ranged priority and no approach to melee reach.

0.1.20 (Nov 22 2026, first online game: stance units stood still): a stance unit under an attack order kept that order in
front (it cannot shoot, so the order never ends) and the dll's melee order waited behind it. `front_kind` 5 = someone else's
eAttack in front: the dll's order takes its place (flags 5).

Test results of this build (after the review fixes, design\reports\REVIEW_fixes.md, and the start-up fix, design\reports\
STARTUP_batch.md): every offline test passes (`build.py all`:
ini check, port check, self-test `ALL OK`, parity `PARITY OK`; section 7); the in-game checks of section 7 have not been run yet.

## 1. What it is

Three modules on one shared core. Each module is its old dll's code plus the edits the build spec records (logging, ini path,
code writes and the new-battle check go through the core); every finder, hook, decision, ini default and log text is the old
one (section 6 lists everything that differs).

| module | was | what it does | acts on |
|---|---|---|---|
| stance | GEM MeleeStance 0.7.42 | melee stance (`melee_toggle` button, tag `melee_stance`), charges, fight-range auto-draw, `melee_infantry` 80 m charge (`[Zerg]`), stance melee on vehicles, multi-select `{instant}` buttons, downed-target ignore, blow reach 45, reload cancel; per-quant tick (also GEM Teleport's, export `gem_quant_listen`) | humans only |
| chase | GEM MeleeChase 0.5.4 | vehicle-brain units (fauna, dreadnoughts, walkers, vehicles) with a melee weapon re-plan their approach to a moving target, lead it, run, hold ranged weapons in melee reach; `ai_prioritize_melee` / `_ranged` props and entity tags | non-human actors only |
| pen | GEM PenFilter 0.1.1 | the AI does not fire `pierce` ammo at vehicle armour it cannot penetrate and aims at the most important part it can (AimPriority groups) | `pierce` ammo vs vehicle targets, melee ammo exempt |
| core | new | one log, one ini, one background thread, patch registry, crash logger, shared new-battle detector | - |
| link | new | integration rules R1-R8 (section 5): flip detector, trace, cease-fire veto, cost line; log only except the veto | - |

Install order inside the dll: chase, stance, pen (their old alphabetical load order). Banner `GEM Melee 0.1.0 (stance 0.7.42,
chase 0.5.4, pen 0.1.1)`; the module banners keep their own texts and versions (`[GEM MeleeStance 0.7.42]`, `GEM MeleeChase
0.5.4`, `[GEM PenFilter 0.1.1]`).

Alex's decisions it keeps (design\map_requirements.md section 2; none of them is changed by the merge):
- Stance: Dawn of War style toggle, tag-driven, no mission triggers (D-S1); blow reach 45 / 45 / 45 (D-S2); nobody fires at or
  charges downed units (D-S3); `[ShieldMelee] Enabled=0` (D-S5); the toggle draws the melee weapon (D-S6); double-click run
  beyond 200 units, `eFight` inside (D-S7); fight range draws melee, stance units also go after vehicles and stay on them
  (D-S8, D-S9, D-S10); units under direct control are left alone (D-S11); melee rows stay below the gun rows (D-S12); fight
  range = melee + fight order, back to the gun when nobody is in range, NoMeleeMs 20000, LeaveFactor 1.5 (D-S13, D-S14);
  SkipTags / IgnoreTargetTags (D-S15); the 80 m `melee_infantry` charge replaces the `melee_inf_atk` triggers (D-S16);
  `[AutoDraw] PlayerUnits=0` (D-S18); a player's attack order is honoured, PlayerMaxDistance 1000 (D-S20); the 0.7.42 no-melee
  rule `AutoMeleeTag=melee_infantry` (D-S21).
- Chase: humans untouched (D-C1); vehicle melee weapons use `veh_melee`, never the word `melee` on a `pattern gun` set (D-C2);
  entity tags `ai_prioritize_melee` / `_ranged` override the def props (D-C3); ranged priority keeps the engine's approach range
  (D-C4).
- Pen: the volume choice is PenFilter's job (D-P1), melee rows at the bottom of the vehicle classes catch its rejections (D-P2),
  aim priority body ; turret, mantlet ; aux turrets ; arms ; legs ; tracks (D-P4).
- One dll for the three (D-M1). Flanking is not in 0.1.0 (D-F1).

Detailed behaviour of each module: the old READMEs, `inputs\meleestance_0.7.42\README.md`, `inputs\meleechase_0.5.4\README.md`,
`inputs\penfilter_0.1.1\README.md` (their ini sections are now sections of gem_melee.ini; their log file names are replaced by
section 3 below).

## 2. Payload swap

1. Remove `gem_meleestance.dll` + `gem_meleestance.ini`, `gem_meleechase.dll` + `gem_meleechase.ini`, `gem_penfilter.dll` +
   `gem_penfilter.ini` from the payload (launcher `launcher\payload\`, or the DEV `dll_dir`). Guga's `build.bat payload` in
   `_dlls\_indomitus_DLL_meleestance` / `_meleechase` copies the old files back in: do not run it for this payload.
2. Add `gem_melee.dll` + `gem_melee.ini` (`out\dll\`). The file name sorts between `gem_itempen` and `gem_noprone`, so it loads
   after GEM Fauna and before GEM Teleport and UserControl, as the old dlls did. The one order change: pen now installs before
   GEM NoProne (harmless: NoProne's finders are string-anchored, no shared bytes).
3. Replace `gem_teleport.dll` with Teleport 0.2.14 (`compat\teleport_0.2.14\dist\gem_teleport.dll`, its `gem_teleport.ini` only
   has one comment changed). 0.2.13 finds the per-quant tick only in a dll named `gem_meleestance.dll`: with gem_melee and 0.2.13
   the jetpack aim events (range rings) fire only when some unit gets an order. 0.2.14 looks for `gem_melee.dll` first, then
   `gem_meleestance.dll`. Teleport is Guga's dll: 0.2.14 is a proposal (source diff `compat\teleport_0.2.14\teleport_0.2.14.patch`).
4. `gem_contactdamage.dll` must stay in the payload: stance units hit vehicles through script contactors whose
   `{vehicle_health_damage_pierce}` line crashes the game and the server without it, on every vehicle melee hit (CS-11).
5. Every machine of a multiplayer match and the server need the same build (it changes AI decisions); the payload fingerprint
   changes, as with any payload change. Guga's live 1.0.3 has MeleeStance 0.7.41 + MeleeChase 0.5.3 and no PenFilter: never mix.
6. Do not rename `gem_melee.dll` while Teleport 0.2.14 is in the payload: Teleport finds `gem_quant_listen` by this file name
   (a rename needs a Teleport rebuild with the new name). The ini and log names follow the dll's own file name.
7. Old and new together: an old dll already loaded when gem_melee loads (an old one in the payload with gem_melee in the DEV
   `dll_dir`) turns the matching module off (`<name> is already loaded in this process: the <mod> module stays off (remove one of
   them)`); with an old gem_meleestance.dll, gem_melee's `gem_quant_listen` passes Teleport's registration on to it. An old dll
   loaded after gem_melee (both in one payload) cannot be seen at load: the core warns within `[Core] LateGuardSeconds` (60 s)
   `WARNING: <name> was loaded after gem_melee.dll: remove it from the payload`; the old stance / pen then find their sites
   patched and install nothing, the old chase installs only its crash logger. Nothing is ever unloaded.

## 3. Files and logs

| file | where |
|---|---|
| `gem_melee.dll`, `gem_melee.ini` | payload folder (`%LocalAppData%\Indomitus Launcher\payload\<fingerprint>\`); a hosted MP server gets its own copy in `<payload>\server\` |
| `gem_melee.log` | game (`call_to_arms.exe`) |
| `gem_melee_editor.log` | editor (exe name contains `_ed`) |
| `gem_melee_server.log` | dedicated server (exe name contains `_server`), in `<payload>\server\` |

The ini name is `<dll path without extension>.ini`; the log name is the same + `_editor` / `_server` + `.log`. Append mode, opened
on the first line, shared read (readable while the game runs). `[Core] Log=0` writes no file at all. The log is opened by its wide
(Unicode) path, so it is written in any payload folder; a fatal-exception line never opens the file itself (it is written only to
a log that is already open). In a folder whose name has characters outside the ANSI code page, chase reads the ini by its wide
path (as MeleeChase did; the core reads the crash logger's `[meleechase] crash_log` / `enabled` the same way) while stance, pen, core and link read it through the ANSI path (as MeleeStance / PenFilter did) and run on
their code defaults there; the banner then says `found by its wide name only` and one line explains it. gem_meleestance.ini,
gem_meleechase.ini and gem_penfilter.ini next to the dll are not read (one `... ignored (gem_melee.ini replaces it)` line each); a
missing gem_melee.ini gives one line naming the code defaults that then apply (section 4).

Line format: `HH:MM:SS.mmm` (wall clock), the sim clock in ms (9 wide; `        -` before the clock is found), the module tag
`[core]` / `[chase]` / `[stance]` / `[pen]` / `[link]`, then the module's own text, unchanged. Chase's own truncated
`gem_meleechase.log`, its debugger copy and its console window are gone; its lines are `[chase]` lines in the one log.

Line families other tools and people look for (the text after the prefix is the old one):
- stance: `hb:` (`_tools\server_tickrate\hb.py` / `simspeed.py`: pass them `gem_melee_server.log`; their regexes use `re.search`,
  so the prefix does not matter), `no-melee:`, `charge:`, `orders:`, `tick:`;
- chase: `melee unit ... approaches ... following the target`, `melee-priority`, `approaches but has no weapon tagged`
  (conquest.md:22);
- pen: `A [`, `B [`, `P [`, `X [`, `new battle [`, `stats [`, `row filter (hook A) installed` and the other install lines;
- new: `[core] --- new battle`, `[core] patches:`, `[core] code writes:` (how many code writes, thread walks and ms at load), `[core] WARNING: <mod> clock`, `[stance] flush for core new battle` / `[pen]
  flush for core new battle`, `[link] flip:` (plus `[link] flip: FlipLines=20 reached for this battle ...`, `[link] flip: N more
  trips in the last battle were not written ...` and `[link] flip: new battle on the stance|chase clock ... (link's own rule)`),
  `[link] trace:`, `[link] cost:`, `[link] R1 owner words:`, `[link] R6 player policy:`, and link's other attach lines: `[link] clock check: core exe+0x.., stance .., chase .., pen ..: equal`, `[link]
  cease-fire: chase clear target exe+0x.. = pen weapon stop exe+0x..: one engine function ...`, `[link] R2 one pick: ...`,
  `[link] R3 cease-fire veto: ...`, `[link] R4 flip detector: ... | R5 trace: ... | R8 cost line: ...` (a mismatch in the first
  three says `WARNING`).

Tools that find the logs by FILE NAME: `_tools\bot_lua_qc\botlog.py` (`collect`, botlog.py:156-167) looks for
`<payload>\server\gem_meleestance_server.log` and `<payload>\gem_meleestance.log`, newest by mtime over every payload folder, and
`report` (botlog.py:387-388) classifies files by the word `meleestance`. After the swap it would silently collect an OLD session's
MeleeStance log from an older payload folder and build ticks.csv / hb verdicts from the wrong match. It must look for
`gem_melee_server.log` / `gem_melee.log` / `gem_melee_editor.log`; the main session patches it before the next bot QC run.

## 4. Settings (gem_melee.ini)

One ini for the three modules: a header, `[Core]`, `[Link]`, then the three old inis with all their sections and comments
(no section name repeats; the same key name in different sections stays separate). Values = what Alex runs today (payload
a1d6ed3c); the one 0.7.42 key, AutoMeleeTag, has its 0.7.42 value. Every key is explained in the ini comments; notes added for the
merge start with `; gem_melee`. GetPrivateProfile rules: first section / first key wins, case-insensitive, no comment after a
value. The ini is generated: `python tools\make_ini.py` writes it, `python tools\make_ini.py --check` verifies it.

The three code-default traps (an ini without these keys, or no ini at all, behaves differently from today):
- `[Charge] CooldownMs=2000`: the code default is 4000.
- `[AutoDraw] ProbeAnyRange=0`: the code default (and the 0.7.42 source ini) is 1; every payload since Guga's 0.9.15 runs 0.
- `[MeleeStance] AutoMeleeTag=melee_infantry,melee_auto` (Alex, Nov 21 2026: `melee_auto` = may melee without the `[Zerg]`
  charge, carried by the 12 assault marine breeds): written out on purpose. The code default is the same word, so the rule is on even
  without the key; empty = 0.7.41 behaviour (every human with a melee weapon may be switched to it). Only 6 MP breeds carry
  `melee_infantry` (open question Q1, section 8).

Sections:
- `[Core]` (new): `Log` (the whole log file on / off), `CrashLog` (fatal exception and process-exit logger; it is MeleeChase's,
  so it also needs MeleeChase's own conditions: `[meleechase] crash_log=1` and `enabled=1`, in a `call_to_arms*` exe; `[meleechase]
  enabled=0` removes it as in 0.5.4), `NewBattleBackMs` (the sim clock stepping back by at least this many ms is a new battle; stance and
  pen flush on it as well as on their own rules), `PatchSuspendThreads` (code writes suspend the other threads; matters only when
  the dll is put into a running server; at load ONE walk over the thread list serves all 13 code writes, the attach batch: the
  loader lock keeps the set of running threads fixed while the dll loads), `LateGuardSeconds` (how long the core looks for old dlls loaded after it).
- `[Link]` (new): the integration switches of section 5 (`Flip*`, `Trace*`, `CeasefireVeto`, `CostLine`). Log only, except
  CeasefireVeto.
- `[MeleeStance]`: the stance itself (tag, AlwaysTag, AutoMeleeTag, melee ammo words), who is left alone (SkipLinked,
  SkipDirectControl, SkipTags, IgnoreTargetTags), the toggle draw (DrawOnToggle, DrawButton, DrawCooldownMs), cost switches
  (HookTiming, FastPass, ClientGate, DiagManual), `Log` (the stance lines; Log=0 no longer cuts the Teleport order link).
- `[Zerg]`: the 80 m charge of AI units tagged `melee_infantry` that hold their melee weapon.
- `[Charge]`: charge orders (double-click run beyond CloseDistance, `eFight` inside, re-order rules, distances, player units).
- `[MultiSelect]`: `{instant}` + `{apply_to_all}` buttons on several selected units.
- `[ShieldMelee]`: engine paired melee through forcefields (off, Alex's 0.6.1 decision).
- `[Downed]`: no weapon is chosen against a unit tagged `start_personage`.
- `[BlowReach]`: the vanilla paired-blow start distances (17 / 29 / 35 -> 45 for every paired fight).
- `[AutoDraw]`: fight-range auto-draw, probe, order poll, the per-quant tick (`Tick=1` is also what gives GEM Teleport its
  per-quant tick), fight order, LeaveFactor; only for units in stance or tagged AutoMeleeTag.
- `[Vehicles]`: stance units attacking vehicles (VehicleMeleeTags, infantry first, hull contact).
- `[ReloadCancel]`: a melee draw cancels a reload, except for weapons tagged with one of KeepTags.
- `[meleechase]`: the chase (melee words `melee` / `veh_melee`, re-path, lead, run mode, hold ranged in melee, weapon priority
  props / tags), `log` / `verbose` / `log_chase` / `crash_log`. Key `console` is no longer read.
- `[PenFilter]`: the hooks (A row filter, B aim check, P aim priority, X exhausted pick + ExhaustStop), PierceTag, ExemptTags,
  AimPriority, caches and holds, PierceRule, PlayerUnits, SkipDirectControl, Facing.

Cost measurement: `[Link] CostLine=1` (with `[MeleeStance] HookTiming=1`) times pen's hook A inside stance's measured engine time
and writes a `[link] cost:` line every 5 s. It is off by default because it adds two timer reads to every hook A call and inflates
stance's "engine MatchAmmo" share on the `hb:` line; set it to 1 only for a cost run (AT-12). The timer's own cost (what an empty
pair of reads reports, measured once at load: `timer bias N ns per pen A call removed` on the attach line) is taken off the pen A
figure and stays in "engine alone"; without that, pen A read about one timer latency (~20 ns) high per call.

## 5. Integration rules R1-R8 (link)

Link never calls into a module except their read-only answers; its tables are fixed-size, direct-mapped, log-only, keyed by
(entity pointer, entity id) and copy every name they keep. Only R3 can change an AI decision.

| rule | what it does in 0.1.0 | switch (shipped) | log |
|---|---|---|---|
| R1 owner | By construction: stance decides only humans, chase only non-human actors, pen only `pierce` ammo vs vehicle targets that is not exempt. At attach link checks that every `[Vehicles] VehicleMeleeTags` word and `[meleechase] melee_tag` is in `[PenFilter] ExemptTags` | always | `[link] R1 owner words: ...` (+ ` \| WARNING: ...` when a word is missing) |
| R2 one pick, one order | The order of the hooks on one weapon pick is unchanged: pen W records the context, stance H1 decides (ignored target, downed target -> no weapon, fast pass, skip rules, no-melee rule, zerg, vehicles, probe), and only MELEE / PASS reach the engine's MatchAmmo, inside which pen A runs | - (structure) | - |
| R3 cease-fire | Chase's hold and pen's X stop call the same engine function through one core helper. Pen's stop is refused for a weapon whose item tags hold chase's melee word (cannot happen with today's data: pen exempts every melee ammo). Chase is never vetoed. R3b (chase hold sparing a weapon pen holds on another target) is not built | `[Link] CeasefireVeto=1` | `R3: pen stop of melee weapon ... refused: the weapon was NOT stopped (...)` (pen writes its X line, `weapon stopped`, before it asks), `R3: weapon ... stopped by chase and pen in the same sim ms` (20 per battle each) |
| R4 flip detector | Front-order changes per unit from two inputs: stance's order poll (human units on its poll list, sampled every PollMs; not the human player's own units outside the stance with `[AutoDraw] PlayerUnits=0`, nor linked, SkipTags or directly controlled units; nothing at all with `[AutoDraw] Enabled=0` or `OrderPoll=0`, or when stance found neither attack-order vtable, eAttack / eAttackBasic; the attach line says which inputs are on and why one is OFF: stance answers it, S21) and every chase approach registration (same order class each time: a vehicle in the 5.8 m hold / chase loop trips). A stance note that repeats the unit's previous class is not counted (stance's unit slot was reused and handed back empty). The 2nd change inside FlipWindowMs makes the unit a suspect; at FlipChanges changes inside the window one block is written and the unit is muted for 10 s; at most 2 blocks per unit per battle and FlipLines for all units, then one `FlipLines=20 reached` line, and the next battle starts with the number of trips not written. Only suspects' picks and verdicts are noted (an ordinary pick costs one slot load and a pointer compare). New battle: the core's detector, or link's own rule (a note's clock stepping back by `[Core] NewBattleBackMs`) when no core clock or no core poll exists | `Flip=1`, `FlipChanges=5`, `FlipWindowMs=3000`, `FlipLines=20` per battle | `[link] flip:` block: the last 8 order changes, stance's last non-PASS verdict, pen's last A verdict, chase's last hold |
| R5 trace | A unit tagged TraceTag gets one line for each: stance weapon pick (every pick, PASS included; charges, draws and stance orders are not traced, their own lines are), chase priority change, approach registration and hold, pen hook A verdict (every call, held ones too) and X. Pen alone fills TraceLines within seconds for a traced vehicle in combat. No mod file sets `ai_trace`: for a test give one unit the tag (an editor script `{tags add "ai_trace"}` on it, or a breed `{tags}` line) | `Trace=0`, `TraceTag=ai_trace`, `TraceLines=2000` per battle | `[link] trace: [N ms] <mod> unit P: ...` |
| R6 player policy | Unchanged per module (stance and pen test direct control differently; chase does not test it); one table line at attach | - | `[link] R6 player policy: ...` |
| R7 MP | Same compiler flags for every file (`-mcpu=baseline -O2 -ffp-contract=off -fno-fast-math`), ClientGate kept, link / core tables log only, chase's new-battle rule stays its own. Accepted stance exceptions: see section 6 | build | `out\obj\<target>\commands.txt` |
| R8 cost | Stance's own hook timing unchanged (`hb:` line). Pen A timed only with CostLine=1, its timer bias taken off | `CostLine=0` | `[link] cost: [N ms] stance own ... \| engine MatchAmmo ... incl. pen A ... (N ns/call, timer bias N ns/call removed) \| engine alone ...` (max 400 lines) |

Flip block (one per trip, `[link]` prefix on every line):
```
flip: [N ms] unit P (human|vehicle|actor, owner N, id 0xN[, traced]) front order changed N times in N ms (FlipChanges=5, FlipWindowMs=3000):
  [N ms] <order class> (vt P)                     <- up to the last 8 changes
  stance: last <verdict> at N ms: ammo "<name>" vs P   (or "none")
  pen: last <ALLOW|REJECT> vs target 0xN at N ms       (or "none")
  chase: last hold at N ms                             (or "none")
  (no more flip blocks for this unit in this battle: 2 per unit)   <- only on a unit's 2nd block
```
For a chase unit the header says `chase approach registered N times` and each change line is `[N ms] chase approach (vt P)`.
A missing ammo or target prints as `none` (`ammo none`, `vs none`, `vs target none`).

## 6. What changed against the three dlls

Simulation-relevant:
1. Stance flushes its per-mission tables also when the core saw the core clock step back by 1-5 s (its own rule needs > 5 s, ms:2274),
   and when the core's per-quant poll or pen's call saw the restart first. (H22: the three detectors disagreed.) Not on its first
   log_epoch call of the session (as before: nothing to flush then).
2. Pen flushes its caches when the core detected a restart even if pen's first call in the new battle comes at a later clock than its
   last call in the old one (its own rule then misses the restart, pf:1047; H22). Not on its first call of the session (as before).
3. Stance runs its flush check at the start of each quant (S3) once its quant hook is in, with ms_tick's own "no mission",
   ClientGate, engine-thread and re-entry tests, so on the host it flushes in the first quant of a new battle instead of at its next
   throttled tick or hook call (units noted by order events in between are no longer wiped, D1). Clients: unchanged (ClientGate).
4. Stance connects to Teleport's `gem_order_listen` also with `[MeleeStance] Log=0` (was: no helper thread at Log=0, ms:3158; OB-9).
   The shipped ini has Log=1, so no change in practice.
5. R3 veto: pen's weapon stop is refused for a weapon whose item tags hold chase's melee word (cannot happen with today's data).
6. Modules turn off when their old dll is already loaded (guard); with an old gem_meleestance.dll, gem_melee's `gem_quant_listen`
   passes Teleport's registration on to it (S19).
7. Chase is compiled with `-mcpu=baseline -ffp-contract=off -fno-fast-math` (bit-identical results, map_chase 6b).
8. (0.1.1, port edit S22; Alex, Nov 21 2026) The melee toggle's switch-off click gives the unit a take-item order for the weapon
   it held at the switch-on click that drew its melee weapon. 0.7.42 gave no order: the unit kept the melee weapon until the
   engine's next weapon pick in combat, and for a human player's unit nothing else asked (`[AutoDraw] PlayerUnits=0`). Only for
   a unit that carries the toggle tag and not AlwaysTag, holds its melee weapon or has a draw of it running, is not linked and
   carries no SkipTags tag. The name is kept in a table of its own, so the order poll reads nothing new. Nothing is cancelled:
   a unit with an order in front takes the gun after it, and a may-melee unit (AutoMeleeTag) with enemies in view can still be
   sent in by `[Charge]` while the melee weapon is in hand. Log `[stance] ranged: ... melee stance switched off: take-item order
   for "<gun>" added`.
9. (0.1.1, port edit S23; Alex, Nov 21 2026) The charge run is re-aimed when the enemy has moved 40 units or 40 % of the
   distance from the ordered spot, and now never more than `[Charge] MoveRefreshMaxUnits` (200 = 10 m; 0 = no upper limit, the
   0.7.42 rule). In 0.7.42 the limit grew with the distance (21 m at 53 m), so a stance unit sent in from far kept running at
   the spot the enemy had left. Inside 25 m nothing changes. The run still goes to where the enemy is, not to where it will
   be (0.1.1; the lead is item 10).
10. (0.1.2, port edit S24; Alex, Nov 21 2026) Intercept lead: the far charge run at enemy infantry goes to where the enemy will
   be (`[Charge] Lead`, `LeadSeconds`, `LeadKeep`, `LeadRunSpeed`, `LeadMaxFraction`). Velocity = position samples 250 ms apart
   per enemy human, taken in the scan hook and averaged with the one before; lead = velocity x the time a runner needs to meet
   it, at most LeadSeconds and at most LeadKeep x the time the enemy has kept its heading, never more than LeadMaxFraction of
   the distance. No usable velocity (standing, a jump, not scanned for 1.5 s, fewer than two moving samples) = the enemy's
   position, as before. A stall re-order runs straight at the enemy and keeps that unit unled for 15 s; vehicles are never
   led; nothing knows the map's edge or what stands on the lead point. Log `  ^ lead: run goal N units ahead of the enemy ...`
   under the charge line. The numbers come from `tools\lead_sim.py` (a model of the rules, not the dll's code);
   `tools\lead_mutants.py` checks that the self-test notices each rule being broken. Table hash `ptr_hash` (high half of a
   64-bit multiply; S22's toggle-gun table uses it too).
11. (0.1.3, port edit S25; Alex, Nov 21 2026 on 0.1.2: "he only updates his pos once he gets to the original location") In-place
   re-aim (`[Charge] Smooth`, `SmoothMs`, `SmoothFraction`). A new run order makes the unit stop and re-path, which is why re-aims
   were rationed. The engine moves a squad follower's goal without that: its "following" state builds an eMoveTask, asks the
   unit's front order `vt[13](front, new)` (started, flags +0x18 bit 0x10000, and the same class) and calls `vt[14](front, new)`,
   which, while the front order is moving (state +0x3C = 3 or 4), takes over the new goal, plans the path and merges it into the
   running path-follow sub-order; the carrier order is released (`vt[4]`) and deleted (`vt[0]`). `smooth_reaim` does the same for
   a running charge run the dll gave, so the goal follows the aim point (SmoothFraction of the distance, at least
   MoveRefreshUnits, every SmoothMs) while the unit keeps running. The method bodies are checked once per session (`smooth_vt`,
   log `in-place re-aim ON / OFF`); `tools\smooth_check.py` shows that the game, editor and server exes agree. A front order that
   is not ours or not started, one that stays in planning for 1.5 s, or a refused merge falls back to a new order under the old
   limits for 10 s. For a NEW order the rule is 'hybrid' in `tools\lead_sim.py`: the running order's own prediction while its
   lead time lasts, then the point a new order would get (0.1.2 kept an order until the enemy was 200 past its goal: re-aims
   2.3-4.1 s apart in Alex's log). Log `charge: ... run re-aimed in place (N so far): goal G units ahead of the enemy; the unit
   ran R units in the T ms before (S units/s)`: S near the run speed means no stop. Not run in game before 0.1.3 went out.
   0.1.4: the merge plans a path, and the engine's planner must run inside the quant. The dll's tick runs at the start of the
   quant executor's function, before that function marks the quant as begun, and a merge from there made the editor log
   `Assertion failed : quantControl.InsideQuant() (ePP_Cost.cpp:386)` once per re-aim with a hitch each (Alex on 0.1.3: works
   really well, but "TANKING my frames"; 25 asserts for 25 re-aims). `charge_flush` now merges only when a unit's weapon-pick
   scan calls it; in the tick such a re-aim is kept for the next scan, and becomes a new order if none comes for 0.6 s.
   0.1.5: the log says why a run at the enemy the unit was already running at became a new order (`  ^ new order, not in place:
   ...`: held off after a failed try and what failed, another order in front, run order not started, no run order on record,
   stall), writes a line when the order refuses a merge, and counts all of it on `hb: re-aims`. Log only (Alex's 0.1.4 run had
   14 such orders and no reason in the log).
   0.1.6: the reason lines of 0.1.5 showed `another order is in front, not the run order` on all 25 such orders of Alex's run. The
   engine's `eActor::AddOrder(actor, order, {flags, stamp})` appends with flags 1 (what the dll always passed), inserts at the front
   with 2 and cancels the queue first with 4; a normal click passes 5 (`tools\re_addorder_flags.py`, the three exes agree). So a new
   run order for a unit still on an older run order of the dll's waited for that run to arrive, and a melee order inside
   CloseDistance waited for the run to end. `[Charge] ReplaceOwn=1`: when the unit's front order is a run order of the dll's own
   chain (`last_t.last_o`, the newest order the dll added, is still in the queue, and the front order is in `own[]` and is not a
   fight / attack order), the first order of the new charge order goes in with flags 5 (the eMoveTask after its eMovementChange
   keeps 1). Inside the quant only (a scan flush); with no scan for 0.6 s it is appended as before. Log `  ^ takes the place of the
   dll's own run order in front (AddOrder flags 5)` under the order line, counts on `hb: re-aims`.
   0.1.7: 0.1.6 still appended when someone else's order was in front (an attack click, an engine order), one order every 1-3 s:
   Alex's log has `melee stance switched off ... unit now has 33 order(s)`, and the units then ran one outdated goal after the other
   ("running off the map"). Now one order at a time (`front_kind`): none in front = added; the dll's own run order or anyone's
   attack order in front = replaced with flags 5; the dll's own fight order or any other order in front = nothing is given (log
   `charge: ... no order, ...`); outside the quant with no scan for 0.6 s = nothing. The stance switched off: the gun's take-item
   order goes in with flags 5 while the dll's orders are still queued (`stop_flags`), or a movement change cancels them when no gun
   goes back (`charge_stop`). In the offline test `ReplaceOwn` is off except in the 0.1.7 block (its older cases expect appended
   orders).
   0.1.8: 0.1.7's "nothing behind another order" kept the marines from charging (the engine's eComplexOrder sits in front of a
   fighting unit nearly all the time; Alex: "1.6 was way better, we just need to fix those dangling orders"). `front_kind` 1 (the
   dll's own run order) = replaced with flags 5 as in 0.1.6; 2-4 (its own fight order, someone's attack order, any other order)
   = appended as in 0.1.6, unless `last_o` is still queued and is not the front order, i.e. one order of the dll's already waits
   there (log `charge: ... no order, one of the dll's already waits behind ...`). A late order (no scan for 0.6 s) is appended again.
   Stance off: `stop_flags` = 5 whenever an order of the dll's is in the queue, also behind someone else's order (that order ends
   with the stance), so nothing of the dll's runs on after the switch.
Chase's new-battle rule is unchanged (it does not use the core detector, D1): it flushes exactly when 0.5.4 does, on every machine.
A module whose clock pointer differs from the core's (`[core] WARNING ... clock` line; none on the 1.069 exes) keeps only its own rule:
then items 1-3 do not apply to it.
Not simulation-relevant: code writes happen with the other threads suspended (no other threads exist at early-bird injection;
safer in the server late-load path), at load with ONE thread walk for all of them (the attach batch; ~0.5 s less per start on
a PC with ~5900 threads than one walk per write, STARTUP_batch.md; one `[core] code writes:` line); one log file with prefixes, opened by its wide path (MeleeStance and PenFilter wrote no log in a
folder whose name is outside the ANSI code page; their ini reads there are unchanged); one background thread; flip / trace / cost
lines (flip and trace notes only for suspects / traced units); the `flush for core new battle` line; registry table. The crash
logger keeps MeleeChase's conditions (`[meleechase] enabled` / `crash_log`, call_to_arms exe) plus `[Core] CrashLog`.
Nothing else changes: every finder, hook, verdict, order, ini default and log text of the three modules is the snapshot's.

R7 note: the core, link, chase and pen keep no address- or wall-clock-dependent simulation state; stance keeps two (pointer-hashed
eviction in `unit_of`, ms:341-344, and the wall-clock tick throttle, ms:1741-1743), as in 0.7.42. They cannot desync (only the
hosting process makes these decisions, ClientGate ms:1737-1740), but they make two server runs of the same match differ, so
run-to-run differences in hosted tests are not merge bugs.

## 7. Tests

Build (`build.bat <target>` = `python tools\build.py <target>`; zig 0.14.0 from `C:\Users\alex_\Documents\IndomitusLauncher\zig`,
every compile with `-target x86_64-windows-gnu -mcpu=baseline -O2 -ffp-contract=off -fno-fast-math`, a build fails on any
`warning:` line, every command line goes to `out\obj\<target>\commands.txt`):

| target | output | pass |
|---|---|---|
| `core-test` | out\wp0\core_test.exe | `ALL OK` (core cases C-1..C-10) |
| `test-stance` / `test-pen` | out\wp1\stance_test.txt, out\wp3\pen_test.txt | byte-identical to `baseline\ms_test_0.7.42.txt` / `baseline\pf_test_0.1.1.txt` |
| `test-chase` | out\wp2\chase_<exe>.txt | identical to `probe\chase\out_<exe>.txt` |
| `link-stance` / `link-chase` / `link-pen` | link only | every reference resolves, no warning |
| `dll` | out\dll\gem_melee.dll (+ gem_melee.ini, README.md) | exports exactly `gem_quant_listen` |
| `test`, `run-test` | out\test\gem_melee_test.exe, gem_melee_itest.exe, out\test\test_output.txt | last line `ALL OK` |
| `parity` | out\parity\parity_report.txt | last line `PARITY OK` |
| `ini-check` / `port-check` | - | `make_ini.py --check` -> `INI OK (..)`, `port_check.py all` -> `PORT CHECK OK` |
| `all` | ini-check, port-check, dll, test, run-test, parity | |

build.py checks once per run that `zig version` prints 0.14.0 and writes `# zig 0.14.0, zig.exe sha256 ...` as the first line of
each commands.txt. `port_check.py` holds every port edit to its reviewed form: each rule must be used, add exactly its recorded
number of lines and match its recorded SHA-256 (a later, reviewed edit is recorded with `python tools\port_check.py --rehash all`).

Self-test `out\test\gem_melee_test.exe` (no arguments = the three 1.069 exes in `binaries\x64`; env `GOH_BIN` overrides the
folder): each step runs in its own child process and prints `== <step>: exit <n>`: `--core` (core cases), `--stance` and `--pen`
(the modules' own self-tests, output byte-identical to the baselines), `--linked` (both module self-tests again with link
attached, every unit a suspect and traced: same output + `LINKED OK`), `--chase <exe>` (finder probe per exe), then
`gem_melee_itest.exe --install <exe>` per exe (dry install of all three modules into a mapped copy of the exe: 26 writes, 0
refused, all clocks equal, and every written jump, call or slot leads to the gem_melee function it must reach: `targets: 25
entries lead to their gem_melee function, 1 data`; the dry run has no attach batch: `code writes: 13 ... 13 thread walks`; the R4
line says the order poll is on because stance can run it; `INSTALL OK`), `--epoch` (new-battle detector and the stance / pen flush terms,
plus stance's quant hook frame_hook itself: core poll and stance flush before the engine's frame function, none on a thread that
is not stance's engine thread; `EPOCH OK`), `--replay` (today's twitch log through the real R4 detector: units
`00000001EB7D9490` and `000000023326A590` flip, `REPLAY OK`), `--link` (R4 incl. the per-unit and per-battle caps, repeated
classes, chase registrations and link's own new-battle rule without a core clock; R5 incl. no core clock; R8 incl. the timer bias;
`LINK OK`). `--linked` also requires stance front-order notes, stance pick notes and pen verdict notes to reach link, and stance's
`--amtag` second pass runs with link attached too.

Parity `python tools\parity.py` (P-1 old test binaries still give the baselines; P-2 merged module tests = old outputs; P-3 dry
install patch set = the old dlls' own install lines and the exe bytes, written in the install order chase, stance, pen; P-3c the
same attach with the attach batch (`gem_melee_itest.exe --attach-time <exe> batch`): the 13 code writes share one thread walk; P-4
every editor address of today's live logs found again; P-5 exports / imports / TLS; P-5b a copy of the shipped dll and its ini loaded
into child python processes: banner with `ini ... found`, modules line, the real attach's batch (`code writes: none (attach batch
open, no thread walk needed)`), no crash logger outside a call_to_arms exe, the early guard
with an empty dll named gem_penfilter.dll loaded first, the late guard's WARNING with it loaded after (written only by the core
background thread); P-7 every compile and link command exactly as spec 1.3 says for that object). `--live <gem_melee_editor.log>
[<gem_teleport.log>]` (P-6) checks a real editor run: the old install addresses, the `tick: ... vtable slot exe+0xF381D0 hooked`
line, `patches: 26 written, 0 refused`, `code writes: 13 with the other threads suspended, 1 thread walk (attach batch, ...`, no
`[core] WARNING`, `[core] crash log ... ON`, a `[stance] hb:` line, and with the Teleport
log `[stance] orders: listening to GEM Teleport's GiveOrder hook` and Teleport's `aim: per-quant tick from gem_melee.dll
(gem_quant_listen) registered` (Teleport registers on the first jump-pack aim or jump order, so the editor run must include one).

Teleport 0.2.14: `python compat\teleport_0.2.14\check.py` (0.2.13 rebuilt from source is still byte-identical to the live dll,
SHA-256 98D4F8FE...052D; the 0.2.14 diff touches only the listed lines; exports of 0.2.14 = {gem_order_listen}).
`python compat\teleport_0.2.14\make_0.2.14.py` regenerates the folder, the patch and `dist\`.

Results of this build (Nov 21 2026, offline, after the review fixes and the start-up fix, design\reports\STARTUP_batch.md;
`python tools\build.py clean`, then `python tools\build.py all` exit 0):
- `python tools\make_ini.py --check`: `INI OK (142 keys)` (every key the three modules' code reads is present: 80 stance + 28 chase
  + 19 pen; the 26 keys gem_melee's own code reads are present too)
- `python compat\teleport_0.2.14\check.py`: `OK` (0.2.13 rebuilt = 98D4F8FE...052D; 0.2.14 `dist\gem_teleport.dll` SHA-256
  B29AD36641F58442B7F415B1A53C6030962F230055359C8397BFEE12E9CF4BC5, 65024 bytes, exports `gem_order_listen`)
- `python tools\snapshot_inputs.py --check`: `originals unchanged since the snapshot`
- `build.bat dll` (exports): `['gem_quant_listen']`; every commands.txt starts `# zig 0.14.0, zig.exe sha256 8cad11fe...ddc18`
- `build.bat run-test` (`out\test\test_output.txt`): all 13 steps `exit 0`: core `ALL OK` (incl. C-3 exact NewBattleBackMs
  boundary and listener order, C-4 thread suspension with and without the attach batch: one walk for a batch's writes, handles
  closed, exited and parked threads); stance / pen self-tests; linked `LINKED OK (notes 27, traces 35)` (each
  source reached link; the `--amtag` pass ran with link); chase x 3; install x 3 `INSTALL OK` (26 written, 0 refused, all four clocks
  and both stop functions equal, `targets: 25 entries lead to their gem_melee function, 1 data, 0 wrong, 0 unknown`); epoch
  `EPOCH OK` (incl. case 7, frame_hook itself); replay `REPLAY OK` (16 flip blocks on 13 units, both required units); link `LINK OK`;
  last line `ALL OK`
- `python tools\parity.py`: `PARITY OK`, 40 checks, 0 FAIL (P-1 references = baselines; P-2 / P-2b merged tests = references; P-3 26
  patches per exe = the references and the exe bytes, in the order chase, stance, pen; P-3c one thread walk with the attach batch,
  per exe; P-4 all 23 + 17 + 36 live editor addresses;
  P-5 exports / KERNEL32 + CRT imports / TLS; P-5b load tests plain / early / late of a copy of the shipped dll; P-7 28 compile and 4
  link lines exactly as spec 1.3)
- `python tools\port_check.py all`: `PORT CHECK OK` (stance 31 hunks incl. S21, chase 7 files + core_crash.c, pen 25 hunks; every
  rule used, with exactly its recorded lines and SHA-256)
- gem_melee.dll SHA-256 / size: 685E572373861F1E1BC593FC139D082128D7A1FD401B3F713D17D163F35EA39F, 270848 bytes (the build is
  deterministic: a copy of the tree in another folder, work\startup\mut\clean_deep, built the same file)
- start-up (work\startup\ab\timing.md): the whole attach on the mapped exes took 1334 / 1232 / 1196 ms (game / editor / server,
  median of 3) with one thread walk per code write and takes 766 / 683 / 636 ms with the attach batch (floor without any
  suspension 750 / 634 / 590 ms); the walks themselves ~590 ms -> ~46 ms, on a PC with ~6200 threads
Not yet run: the editor / game / server checks below (T4 / T5).

In-game checks (Alex; test launcher = today's payload with the swap of section 2, logs in the payload folder; expected lines are
the old texts with a module prefix):

| # | steps | expected |
|---|---|---|
| AT-0 start | open the editor through the launcher | one banner `GEM Melee 0.1.0`; stance install lines (`ammo match exe+0x7AADA0`, charge, multi-unit 9/9 vtables, auto-draw, vehicles, reload cancel, direct control, downed, blow reach 17/29/35 -> 45, `no-melee rule ON`); chase lines containing `humans` + `not touched`, `vehicle melee chase` + `ON`, `weapon priority entity tags` + `ON` (MeleeChase pads its names with spaces: `humans                         not touched`); pen hooks A / B / P / X installed; `[core] patches: 26 written, 0 refused`; `[core] code writes: 13 with the other threads suspended, 1 thread walk (attach batch, ...)`; no OFF / FAILED / not found except the server-only manager line. Teleport log: `order listeners: callback ... registered`; after the first jump-pack action is aimed or a jump is ordered: `aim: per-quant tick from gem_melee.dll (gem_quant_listen) registered`. After play: `[stance] tick: .?AVeQuantExecutor@Scene@@ ... hooked` |
| AT-1 toggle + charge | 5 assault marines, melee toggle, double-click an enemy squad 40 m off | popup On on all 5, melee icon; `draw:` lines; `charge: ... run`, then melee inside 10 m; toggle again: Off, ranged icon |
| AT-2 fight range (AI) | AI tactical marines (knife on the belt) meet AI guardsmen at 5 m | knife out within ~1 s, fight order, gun back beyond 7.5 m - only for breeds AutoMeleeTag lets melee (Q1) |
| AT-3 scouts | SM scout with chainsword / combat knife in stance | swings |
| AT-4 melee_infantry bots | `dg_melee` squad (AI) 60 m from enemy infantry | runs in and fights (`[Zerg]`) |
| AT-5 twitch | CSM bolter squad vs a hostile SM dread on test.mi, 2 min | `no-melee:` lines. The merge does not change the weapon pick these marines get (0.7.41 refused the melee row through the vehicle rule, 0.7.42 through the no-melee rule: the engine sees NULL either way), so a squad that still flips is not a merge regression: then `[link] flip:` blocks show it, with pen's last verdict and stance's `no-melee REJECT`. No block = they do not flip, unless a `[link] flip: FlipLines=20 reached` line came first (a looping dread trips at most 2 blocks per battle) |
| AT-6 stance vs tank | stance squad on a Leman Russ | `attack-vehicle`, `pressed against`, swings land, Russ dies, no running round the hull |
| AT-7 dread vs Russ | AI assault-cannon dread vs AI Russ | `[pen] A ... REJECT` for the AC drum at the front; `[chase] melee-priority unit P: target ... approach range ... -> melee reach` and `[chase] melee unit P approaches (...) - following the target`; it punches; no 5.8 m loop (if it loops: a `[link] flip:` block `chase approach registered N times`) |
| AT-8 Russ vs dread | AI Russ (AP + HE) vs a dread | AP / HE changes at most once per 8 s, no reload every second, `P ... aim group` lines |
| AT-9 broken arm | break a ranged dread's `gun_right` | `[chase] ... entity tag "ai_prioritize_melee" -> melee priority`; it charges; repair -> ranged again |
| AT-10 direct control | drive a stance marine mid-fight, then a Russ under pen | no swap / charge for the driven unit (`manual:` lines); `direct control skips` count up in `[pen] stats`; normal again after release |
| AT-11 restart | stop and restart the scene twice | one `[core] --- new battle` line per restart; with the quant hook in, stance's `--- clock` line in the same quant (same sim field; preceded by `flush for core new battle` when stance's own > 5 s rule did not fire); pen's `new battle [` line at its next hook call (with the same extra line when its own rule did not fire); chase's `simulation clock ... new mission` line from its own rule at its next vehicle tick; first pen verdicts not `held since` an old time |
| AT-12 cost | 10 min bot battle with `[Link] CostLine=1` | `hb:` dll time per call not above the 0.7.41 + PenFilter numbers (~0.5-2.3 us dll, ~1.6 us engine incl. pen); stance's own ns per call with Flip=1 within 2% of a Flip=0 run; `[link] cost:` lines |
| AT-13 hosted MP | host a 2-player game with bots, melee squads, a dread and tanks, 15+ min, end the match, play a second battle | `server\gem_melee_server.log` has the install lines; the client's log shows the tick gated and no `[pen] A [`, `P [`, `B [`, `X [` or `stats [` line (pen idle on clients: whether its aim-picker hooks run there is not traced); no crash at match end or in the second battle; jetpack jumps show on the client; the client's toggle and charges work; no desync messages in game.log |

## 8. Known open items

- Q1 AutoMeleeTag: shipped as `melee_infantry,melee_auto` (Alex, Nov 21 2026). `melee_auto` is on the 12 assault marine breeds
  (they draw at fight range as bots, no 80 m charge; the thunder-hammer veteran now spawns pistol in hand). Before that:
  shipped as `melee_infantry` (0.7.42 default, Alex's Nov 20 rule; orchestrator decision). Only 6 MP breed files
  carry the tag (bp / dg cultist5, bp_storm_melee, dg_melee, dg_vet_melee): assault marines, raptors, ork boyz / grots, cultist6,
  commissars and officers never melee outside the stance until their breeds are tagged. Empty = 0.7.41 behaviour.
- Q2 `[AutoDraw] ProbeAnyRange=0` (live payload) and `[Charge] CooldownMs=2000` shipped (the values Alex runs).
- Q3 R3b (chase's hold sparing a weapon pen holds on a pierceable volume of a second target) is not built: it would change
  Alex's hold design.
- Q4 Chase keeps acting on player-owned and directly controlled vehicle-brain units (no check, as in 0.5.4).
- Q5 Teleport 0.2.14 is Guga's dll: built under `compat\teleport_0.2.14\` and used in the test launcher; who ships it is open.
- Q6 The quant-hook install on the 1.069 game and server exes is unproven (only the editor is logged): the T4 / T5 runs must show
  the `tick:` line.
- Q7 The MeleeChase 5.8 m hold / chase loop (OB-5) and the CSM twitch (OB-1) are carried unchanged; the R4 `flip:` block is the
  tool to look at them in the next test (FlipChanges=5 / FlipWindowMs=3000 trips on 13 units in a replay of today's twitch log,
  the old 6 / 2000 on none).
- Q8 Chase keeps its own new-battle rule instead of the shared core detector (its reset must not depend on which hooks ran on a
  machine).
- `_tools\bot_lua_qc\botlog.py` still looks for `gem_meleestance*.log` (section 3): patch it before the next bot QC run.
- Ini words no mod file uses (Nov 20 2026 scan of resource\set, properties, entity, map, script): `AlwaysTag=melee_always`,
  `melee_disabled` (SkipTags / IgnoreTargetTags), the `melee_auto` example, `TraceTag=ai_trace`; `disable_melee` is only in two
  test maps and sits in SkipTags but not in IgnoreTargetTags (such a unit is left alone as a shooter but still charged and fought
  as a target). All inherited from MeleeStance 0.7.42 except ai_trace; adding `disable_melee` to IgnoreTargetTags would change who
  is attacked: Alex's call.
- Launcher card: a `[gem_melee]` section for Guga's `launcher\res\features.txt` is proposed in
  `compat\launcher_features_gem_melee.txt`; the old `[gem_meleestance]` / `[gem_meleechase]` cards then show nothing.
