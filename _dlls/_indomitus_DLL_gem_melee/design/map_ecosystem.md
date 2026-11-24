# map_ecosystem: what around MeleeStance / MeleeChase / PenFilter the gem_melee merge can break

Written Nov 20 2026 for the gem_melee merge (plan `_notes\gem_melee_merge_plan.md`). Read-only work except the
files under `_tools\gem_melee\compat\` named below.

Conventions
- Addresses are editor 1.069 (`call_to_arms_ed.exe`, TimeDateStamp 6AFC6E14) RVAs as the payload logs print them
  (`exe+0x...`). Game 6AFC6D57 / server 6AFC6DAF addresses only where a source gives them.
- `ms:N` = `inputs\meleestance_0.7.42\src\gem_meleestance.c` line N. `mc/<file>:N` = `inputs\meleechase_0.5.4\src\mc\<file>`.
  `pf:N` = `inputs\penfilter_0.1.1\gem_penfilter.c`. `tp/<file>:N` = `inputs\teleport_0.2.13\src\tp\<file>`.
  `L:N` = `Documents\IndomitusLauncher_mc054\launcher\main.cpp`. Other payload dll sources: `Documents\IndomitusLauncher_mc054\_dlls\<folder>\...`
  (their dlls in a1d6ed3c are byte-identical to that worktree's `launcher\payload\` copies, hashes compared Nov 20;
  SmokeLauncher's source is `_tools\gem_smokelauncher`).
- `brief:N` = `_notes\melee_dll_merge_brief.md` line N (the dread session's fact brief, Nov 20).
- "log:<dll>" = the dll's log in `%LocalAppData%\Indomitus Launcher\payload\a1d6ed3c\` (12:16 editor run).
- "inferred" = not proven by code I read.

Tools written for this map (all read-only on their inputs, main-guarded):
- `compat\scan\eco_scan.py` -> `compat\scan\eco_scan_a1d6ed3c.txt`: payload fingerprint check + every `exe+0x` address in every
  a1d6ed3c log, mapped to its containing function through the editor exe's `.pdata`, grouped by function.
- `compat\scan\vt_slots.py`: which vtables (RTTI class, slot index) hold a given function in the editor exe file.
- `compat\scan\pe_info.py`: exports / imports / TimeDateStamp of a PE.
- `compat\scan\ini_table.py` -> `compat\scan\ini_tables.md` (also pasted into section 6).
- `compat\teleport_rebuild\` (section 4).

---------------------------------------------------------------------------------------------------------------------

## 1. Other payload dlls: shared and adjacent engine sites

### 1.1 Method and coverage
- a1d6ed3c holds 29 dlls (listed in 2.1). 26 are not the three. Their logs (where they write one) were scanned for
  every `exe+0x` address; each address was put into its function with the editor exe's `.pdata` (65904 entries).
  Result file: `compat\scan\eco_scan_a1d6ed3c.txt`. Only 8 functions / globals are named both by one of the three
  and by another dll (scan section "same address"): `exe+0x10A3B0`, `0x3E4E60`, `0x3E60D0`, `0x67E750`, `0x695600`,
  `0x788C00`, `.data 0x13DF988`, `.data 0x142C678`, plus one function where both patch/read different bytes
  (`exe+0x82E010`). Neighbours within 0x80 bytes: `0x695640` (SmokeLauncher) next to `0x695600`; `0x82F430` (Fauna) 0x5C after
  `0x82F3D4` (MeleeChase anchor); clock `0x13DF988` 0x60 after stance's order stamp `0x13DF928`.
- Logs do not print every vtable hook, so vtable-level overlap was checked by source reading plus `vt_slots.py` on the
  editor exe file (results quoted in the rows).
- Source grep over every payload dll source for the engine words the three use (MatchAmmo, eMoveTask, eVS_Root,
  QuantExecutor, eMovementChange, GetCurrentWeapon, SetTarget, IsManualControl, eOutdoorsBattle): hits only in the
  three, Teleport (QuantExecutor: consumer comment, tp/teleport.c:461), Fauna (eMovementChange, sprint/squads), SquadFollow
  README (a "not touched" list). No other dll names MatchAmmo, its ctx builder, the volume picker, the enemy scan,
  the vehicle target evaluation or eMoveTask.

### 1.2 Table of shared / adjacent sites

| # | Engine site (editor 1.069) | Other payload dll: what it does (evidence) | gem_melee part: what it does (evidence) | Load order today | Install order matters? |
|---|---|---|---|---|---|
| S1 | `Entity::GiveOrder` = `eActor::AddOrder` exe+0x67E750 (fn ends 0x67E8C1). Same function in both namings: log:gem_teleport "lock: Entity::GiveOrder exe+0x67E750"; log:stance "AddOrder exe+0x67E750". | **Teleport** patches the prologue with a 5-byte jmp when `lock_orders=1` (tp/teleport.c:611-613, tp/patch.c:82-105); finds it from eCustom vt slot 17's tail `jmp` and checks `mov rcx,[rcx+0xDC8]` in its first 0x20 bytes (tp/engine.c:266-296). Its hook calls the `gem_order_listen` callbacks first, then refuses + deletes orders for teleporting units (tp/teleport.c:564-578). **Fauna** redirects the rel32 of the move action's `call GiveOrder` at exe+0xAB1A2A to its squad-pace wrapper (fx/main.c:104-105; log:gem_fauna line 17). | **Stance** calls it for every order it gives (`g_addorder`, ms:932, 942, 1262, 1557, 2218, 2326). Its finder only takes the rel32 of the UI attack command site (ms:2552-2562) and compares rel32 targets in the eFight finder (ms:2616-2626); it never reads the function's bytes. Chase and Pen: not referenced. | Fauna (8) < stance (13) < Teleport (26) | **No** for installing (stance reads call targets only). **Yes if gem_melee ever inline-hooked 0x67E750**: it loads before Teleport, so Teleport's `is_give_order` / `prologue_len` checks would fail and the lock AND the order listeners would go OFF (tp/engine.c:271-275, tp/patch.c:84-88). Runtime: every stance order goes through Teleport's lock (returns 0 for a teleporting unit; stance counts it as refused) and is reported to the listeners, stance's own `order_event` included; that callback ignores calls while `g_in_tick` or `g_in_hook` (ms:1870), but the button draw path (`draw_melee`, ms:2313-2333, from `ms_exinst`) gives its take-item order outside both, so stance hears its own order (it only notes the unit and forces a poll, no order; harmless). |
| S2 | eMovementChange factory exe+0x788C00 | **Fauna** redirects the move action's `call MovementChange` at exe+0xAB1A18 to a pad from `alloc_near` (VirtualAlloc, outside the image) (fx/main.c:104; log:gem_fauna line 17). Teleport's fallback GiveOrder finder uses the same site pattern and its comment says Fauna's redirect breaks that fallback (tp/engine.c:266-270); the primary path worked today. | **Stance** finds it from every `8B 14 90 E8` (mov edx,[rax+rdx*4]; call X) site whose target allocates 0x48; targets outside .text are skipped, all in-text targets must agree (ms:2627-2637); calls it for charge orders. | Fauna before stance | No (a redirected site points outside .text and is skipped; live log found exe+0x788C00 with Fauna already patched). Would break only if a dll redirected such a site to a pad INSIDE .text (inferred). |
| S3 | `Actor::eVS_Root` vtable exe+0xF76E10: slot 3 = tick exe+0x82F700, slot 4 = exe+0x82E010 (vt_slots.py) | **Fauna** redirects two call sites inside slot 4: exe+0x82F1C2 (formation root re-target, fx/follow.c:113) and exe+0x82F430 (dodge crush condition, fx/dodge.c:149). RepairFilter / ReviveFilter use the same "has tag no_auto_repair" anchor read-only (brief:720). | **Chase** vt_hooks slot 3 (mc/melee.c:1059), redirects the approach call exe+0x82FE14 inside slot 3 (mc/melee.c:1058), and reads slot 4's no_auto_repair test at exe+0x82F3D4 as its entity-tag anchor (mc/melee.c:554-603; byte window p-0x10..p+0x18, mc/melee.c:575-580). | Fauna before chase | No: disjoint bytes (0x82F430 is 0x5C past the anchor) and slot 3 is a vtable write. Runtime: a fauna unit gets Fauna's formation / dodge (slot 4) and Chase's approach / hold (slot 3) together. |
| S4 | `Actor::eChassisFoot` vtable exe+0xF63A38: slot 45 (+0x168) SetMovementMode, slot 44 SetMovementSpeed | **Fauna** vt_hooks both (fx/sprint.c:217-224); its wrapper turns a mode 1 / 3 request into 2 for a sprinting fauna and then commits the chassis mode (fx/sprint.c:192-203). | **Chase** calls `chassis->vt[+0x168](chassis, chase_run_mode, 0)` on every chased unit (mc/melee.c:915-919); the 0x168 comes from eMovementChange vt[19] (mc/melee.c:480-492), which no dll patches. | Fauna before chase | No (runtime vtable dispatch). For fauna units (foot chassis) Chase's run-mode call always goes through Fauna's wrapper + `foot_commit`. |
| S5 | Action vtables, slot 19 = ExecuteInstant exe+0xAA59F0, in 9 classes: eCustom exe+0xFBEA70, eFortify, e_drop, e_pour_in, e_pour_out, e_fortification, e_care, e_injection, e_airstrike exe+0xFC0C30 (vt_slots.py) | **Teleport** vt_hooks **eCustom slot 15** (IsApplicable exe+0xAA5930) and reads eCustom slots 17 and 31 and eCustomActionOrder slot 19 (tp/teleport.c:609; tp/engine.c:192-222, 276-282); it accepts slot 15 already hooked (tp/engine.c:200-206). **BombBay** hooks the eAirstrike order / eBombDropOrder vtables (log: slots exe+0xEF3850 / 0xEF3780), not the e_airstrike action vtable. | **Stance** writes slot 19 in all 9 vtables at DllMain (ms:3205-3226); at click time `ms_exinst` swaps the action object's vtable pointer for a heap copy around each per-unit ExecuteInstant (ms:2351-2361). | Stance before Teleport | No: disjoint slots. Note the per-click vtable-pointer swap: another dll that identified an eCustom action by its vtable pointer during ExecuteInstant would see the heap copy (Teleport does not: it checks instruction objects, tp/teleport.c:440). |
| S6 | `Scene::eQuantExecutor` vtable exe+0xF381C8 slot 1 (pointer at exe+0xF381D0 -> run-quant exe+0x413560) | **Teleport** consumes it only through `gem_quant_listen` and states it must not hook it (tp/teleport.c:458-463). No other dll names it (scan). | **Stance** owns it: lazy one-shot vtable write from `frame_hook_try` (ms:1805-1862), first called from the scan hook (ms:1986-1987) or an order event (ms:1873). | - | No. See C1 for what depends on it. |
| S7 | GetCurrentWeapon exe+0x695600; eWeaponry main weapon getter exe+0x695640 | **WeaponScript (Guidance)** finds GetHeldItem = exe+0x695600 from Bullet::Quant and calls it (gem_guidance.c:94-99). **SmokeLauncher** calls exe+0x695640 (gem_smokelauncher.c:176-177). | **Stance** finds 0x695600 by signature `SIG_CURWEAP` and calls it (ms:2926); **Chase** finds it through an eOutdoorsBattle slot and calls it (mc/melee.c:338-354). | WeaponScript, SmokeLauncher after | No: nobody patches either function. |
| S8 | HasTag exe+0x10A3B0 | **Fauna** calls it (log:gem_fauna line 34). | **Chase** reads its first 0x40 bytes in the entity-tag finder (mc/melee.c:582-593). | Fauna before | No (read / call only). |
| S9 | Tag functions: TagContainer ctor exe+0x3E4E60, FromString exe+0x3E60D0; the `{tags}` script action's Execute (vtable slot 1, exe+0x5D5220) | **UserControl** patches the `{tags}` action slot only on the first SetPlayer call (`LateTagsHook=1`) because BreedTags, TeamTags, SensorTags, MeleeStance and WeaponScript read that slot's code at start-up (usercontrol README.md:64-69; log:gem_usercontrol "{tags} hook installed (slot was exe+0x5D5220)"). TeamTags / UserControl call the same ctor / FromString (logs). | **Stance** reads the `{tags}` slot code at DllMain to find ctor + FromString (`find_tagcont`, ms:2874; log:stance line 465). | UserControl after | Not with LateTagsHook=1. If UserControl's `LateTagsHook=0` were used, its early patch still comes after gem_melee alphabetically: still safe. |
| S10 | Sim clock dword .data exe+0x13DF988; stance's order stamp .data exe+0x13DF928 | **BoardFilter**, **SquadFollow** read the clock (logs). | Stance (ms:2600-2614), Chase (mc/melee.c:422-431), Pen (pf:627-635) find and read it; stance alone reads the stamp. | - | No (read-only data). |
| S11 | Manual-control manager global .data exe+0x142C678 | **Fauna** UI reads `[[gc]+0xD8]` (log:gem_fauna line 45). | Stance reads `[mgr]` actor at +0x3B0 (ms:2928-2931; log:stance line 469). | - | No (read-only). |
| S12 | Brain flags `[[entity+0xDC8]+0x398]` (runtime data) | **UserControl** writes bits 0-1 (control 0 = AI, 1 = user; README.md:72-76). **SensorTags** clears / sets bit 15 (targetable) for `no_target` (gem_sensortags.c:30-39). **Fauna** reads bits 6-9 (sprint.c:9-11). | Stance and Pen read bits 0-1 as "unit of a human player" for their PlayerUnits keys (`(br[0x398] & 3) == 1`: ms:251, 617, 641, 1007, 1297, 1315; pf:733-734). The engine's own MatchAmmo also tests it (`{enable_for_user}`, ms:35). | - | No install effect. Behaviour: a unit tagged `user_control` counts as a player unit for `[AutoDraw] PlayerUnits=0`, `[Charge] PlayerUnits`, `[PenFilter] PlayerUnits`. |
| S13 | exe IAT entries of ExitProcess / TerminateProcess; vectored exception handlers | **Fauna** patches 4 imports first (log:gem_fauna line 3); **ScriptExt** sees them already hooked (log:gem_scriptext line 3). Fauna, ScriptExt and Teleport register `AddVectoredExceptionHandler(1, ...)` crash loggers (logs "crash log ON"; tp/main.c:54). | **Chase** `install_exit_hooks` compares IAT entries with kernel32's own addresses (mc/crashlog.c:59-89): live result "0 import(s) hooked" (log:gem_meleechase line 3); its VEH logs and returns CONTINUE_SEARCH (mc/crashlog.c:91-113). Stance and Pen register no VEH / IAT hook (grep). | Fauna before chase | No while Fauna stays earlier (Chase's exit trace is a no-op then). VEH order only changes which log lines come first. |
| S14 | MatchAmmo exe+0x7AADA0, its ctx builder exe+0x7AACC0, volume picker fn exe+0x7A9A30 (P 0x7A9BFA, B 0x7A9D0D, X 0x7A9D49), enemy scan exe+0x7EF410, weapon stop exe+0x699CC0, vehicle target evaluation exe+0x828240 (entered by a tail jmp at exe+0x827927 in eVehicleBrain / eVehicleBrainClient vt slot 35 = exe+0x827760, vt_slots.py), eMoveTask vtable exe+0xEA16E0 slots 0 / 19, blow table .rdata exe+0xF71368 | none (scan + grep) | the three only (intra-merge, other maps) | - | Only among the three. |
| S15 | NoProne: 14-byte jmp at CanTakePose exe+0x7DA420 (log:gem_noprone) | NoProne's finders are anchored on the strings "cannot_lie", the ApplyBreed string and "tags" (gem_noprone.c:212-261), not on bytes the three patch. | Pen's finders never read CanTakePose; they were self-tested on the unpatched exes from disk (`out\selftest.txt`: ALL OK on game / editor / server). Chase's held-item tag pattern (mc/melee.c:265-282) also matches inside CanTakePose's body, not its first 14 bytes (inferred from gem_noprone.c:224-234: the reader sits after the prologue). | Today pen AFTER NoProne; merged pen BEFORE it | No (this is the only relative-order change, see 2.2). |
| S16 | ItemPierce: HitVolume pierce site exe+0x6C76D2, BulletStep dedupe exe+0x6C5CFF (log:gem_itempen) | patches those two sites | Pen calls the armour sub-functions exe+0x675C40..0x677650 and finds them from the sight overlay AimInfo call sequence (pf:500-564), not from HitVolume | ItemPierce before | No (disjoint). |
| S17 | SquadFollow: 14-byte jmp replacing NeedRefollow exe+0x7E8990 (log:gem_squadfollow) | patches that leaf function | Chase reads eOutdoorsBattle slot code at exe+0x7E1A10 / 0x7E1290 (finder only) - other functions | SquadFollow after | No. SquadFollow README.md:57-60 lists MeleeChase as not touching it. Runtime interplay inferred only: stance charge orders on squad followers vs SquadFollow's re-follow orders. |

Dlls with no shared or adjacent site (log banners + sources): BombBay 0.7 (SpawnBullet / airstrike vtables), BreedTags 0.7.1
(ApplyBreed exe+0x7F6710), ConquestUI 0.4.3 and ResearchPath 0.4.0 (inert in editor and server), ContactDamage 0.1.0
(Pierce vtable exe+0xF51FC0), CostRed, CrewVoice 0.1 (talk call exe+0x7B2EE8), HmapGuard 0.1, HostServer (game-side
CreateProcess path; no log in the editor), RepairFilter 0.2 / ReviveFilter 0.1 (order Execute exe+0x791420 / 0x269B40),
ScriptExt 0.10.1 (condition registry, eHealthDamage* slots, talk events), SensorTags 0.4 (eSensor slots 11 / 13),
SessionDump (Enabled=0), SmokeLauncher 0.1 (eSmokeScreenExtender slot 9), TeamSound 0.3, TeamTags 0.1 (SetPlayer
+0x1D0, Unpack +0x238), UnitName 0.1, UserControl 0.1 (SetPlayer chain, `{tags}` slot), WeaponScript 1.1.2
(SpawnBullet chain, Bullet::Launch / Quant). Not in the payload but in `_tools`: gem_trafficlog 0.1 (README: hooks
nothing, reads `mp_trafficLog`), gem_botlink (uses MeleeStance's order recipe; not in any payload).

### 1.3 Feature dependencies across dlls (not code sites, but a merge can break them)

| # | Dependency | Evidence |
|---|---|---|
| B1 | Stance `[MultiSelect]` is what makes `{instant}` + `{apply_to_all}` buttons act on every selected unit: melee toggle, WeaponScript's overcharge, jetpack. Stance module off / finder failure = those buttons act on one unit. | features.txt:27; ms:3205-3226, 2335-2363 |
| B2 | Breed tags (`melee_infantry` = `[MeleeStance] AutoMeleeTag` and `[Zerg] Tag`, `astartes`, ...) are entity tags only through BreedTags. | log:gem_breedtags lines 9-29; CLAUDE.md "Breed tags ARE entity tags" |
| B3 | Melee damage on vehicles (stance vehicle melee, chase dreadnought fists) arrives through script contactors that call `{vehicle_health_damage_pierce}` + `melee_body_<N>`: needs ContactDamage, crashes without it. | features.txt:40-45; log:gem_contactdamage (12:27 dread hits) |
| B4 | `sensor_off` / `no_target` (SensorTags) remove units from the engine's sensor results; stance additionally skips `sensor_off` in SkipTags / IgnoreTargetTags. | gem_sensortags.c:3-39; ini_tables (stance SkipTags) |
| B5 | Teleport's lock refuses stance's orders for units mid-jump (`tp_jetpack` is also in stance SkipTags; a reload cancel would kill the jump order, ms:2317-2319). | tp/teleport.c:571-576 |
| B6 | WeaponScript Overcharge swaps the fired ammo at SpawnBullet ("plasmagun_mag chaos ammo" -> "... overcharge chaos ammo", log:gem_weaponscript line 56). Pen judges the ammo MatchAmmo matched / the loaded ammo (exe+0x69B440), so the overcharge sibling's penetration is not what pen evaluated (inferred). | log:gem_weaponscript; pf:1723-1727 |
| B7 | Fauna units are vehicle-brain units: Chase acts on them (features.txt:31-38) and S3 / S4 apply. | |
| B8 | UserControl's control bits change the player-unit policy of stance and pen (S12). | |
| B9 | The launcher writes hotkey `actions/melee_toggle=SHIFT + KEY_C` into the game profile (gem_launcher.ini:38); stance's `[MeleeStance] DrawButton=melee_toggle` must keep naming the same action. | gem_launcher.ini:38; ms:3017 |

---------------------------------------------------------------------------------------------------------------------

## 2. Injection order

### 2.1 How the order is made (evidence)
- Manifest = every `*.dll` / `*.ini` in `launcher\payload\`, sorted by name: `build_local.py` sorts with `key=str.lower`
  (`_tools\launcher_build\build_local.py:24-28`); Guga's `build_zig.bat` uses `dir /b /a-d-h-s /on ... *.dll ... *.ini`
  (build_zig.bat:68-76).
- The launcher writes the files in manifest order and collects the `.dll` paths in that order (L:555-572); the
  fingerprint = first 8 hex of SHA-256 over the manifest's sha256 strings (L:594-596).
- Proof for a1d6ed3c: `eco_scan.py` recomputed the fingerprint over the 58 files of the live folder sorted
  case-insensitively and got **a1d6ed3c** exactly.
- Injection: one `QueueUserAPC(LoadLibraryW, path)` per dll on the suspended main thread, in list order; APCs run in
  queue order (L:1120-1136, comment L:9-10).
- a1d6ed3c dll order (29): boardfilter, bombbay, breedtags, conquestui, contactdamage, costred, crewvoice, fauna,
  hmapguard, hostserver, itempen, **meleechase, meleestance**, noprone, **penfilter**, repairfilter, researchpath,
  revivefilter, scriptext, sensortags, sessiondump, smokelauncher, squadfollow, teamsound, teamtags, teleport,
  unitname, usercontrol, weaponscript.
- DEV launcher builds append the dlls of `dll_dir` (`--dll-dir`) AFTER the payload, alphabetical among themselves
  (L:612-613).
- MP host server, way 1 (normal, needs gem_hostserver): the game creates the server frozen, the launcher writes its own
  copy of the payload to `<payload>\server\` and queues the same APCs in payload order (L:1371-1393, 1442-1454,
  1578-1607). Way 2 (backup: server already running): one `CreateRemoteThread(LoadLibraryW)` per missing dll, order
  `gem_scriptext`, `gem_teleport` first, then payload order (`kServerFirst`, L:1527-1541; L:1543-1576).

### 2.2 What changes for each part
Merged: `... gem_hostserver, gem_itempen, gem_melee, gem_noprone, gem_repairfilter ...` ("gem_melee." sorts before
"gem_meleechase" / "gem_meleestance" because '.' 0x2E < 'c' / 's'; any collation puts it after gem_itempen and before
gem_noprone).

| Part | Before it today | After it today | Merged | Change |
|---|---|---|---|---|
| chase | ... itempen | stance, noprone, pen, ... | inside gem_melee, first (plan) | none |
| stance | ... itempen, chase | noprone, pen, ... | inside gem_melee, after chase | none |
| pen | ... itempen, chase, stance, **noprone** | repairfilter ... | inside gem_melee, after stance | **pen now runs before NoProne** (only change in the whole payload) |

Does anything in section 1 make that matter? No: S15 (NoProne string-anchored, pen finders proven on unpatched exes, no
shared bytes). Everything that loads before / after each part is otherwise the same, so S1-S17 keep their current
order. Server way 2 puts Teleport (and ScriptExt) before gem_melee, exactly as it does for meleestance today.

Order-sensitive facts the merged name must keep:
- gem_melee must load **after Fauna** (S2 skip rule, S3 shared function, S13 exit trace) and **before Teleport** (S1: Teleport
  must see GiveOrder unpatched by us; S5: Teleport hooks slot 15 after stance's slot 19) and **before UserControl** (S9).
  Any name between `gem_itempen` and `gem_noprone` does this.
- gem_melee loads BEFORE any file named gem_meleechase / gem_meleestance / gem_penfilter. A DllMain-time guard (plan
  item 7) can therefore only see old dlls that load EARLIER: the DEV `dll_dir` case with a test gem_melee.dll over a
  payload that still has the old dlls (dll_dir comes after the payload). Old dlls that load LATER (both in one payload,
  `gem_inject.exe` / `inject.bat` hand injection, an old dll in `dll_dir`) are not visible from DllMain. What the old
  ones then do, from their finders (inferred from code, not run): old stance finds the MatchAmmo prologue changed and
  installs nothing (ms:2858, 2868: "already patched"), but its `gem_quant_listen` export is still there and Teleport
  0.2.13 would register into that inert copy; old pen finds its patched sites missing (pf:418, 463, 569, 580) and
  installs nothing; old chase sees eVS_Root slot 3 / eMoveTask slots outside .text and leaves the vehicle hooks off
  (mc/melee.c:369-370, 411) but still adds its VEH + exit trace.
- A forwarder named gem_meleestance.dll (plan item 6 fallback) would load right after gem_melee.dll and would trip a
  guard that looks for that module name later (e.g. from the tick), unless the guard recognises the forwarder.

### 2.3 Places that name gem_meleestance / gem_meleechase / gem_penfilter
| Place | Names | Effect for the merge |
|---|---|---|
| `IndomitusLauncher_mc054\launcher\main.cpp` | none of the three (only gem_launcher, gem_lobbycheck, gem_hostserver, gem_scriptext, gem_teleport; grep) | nothing to change; `kServerFirst` = scriptext, teleport (L:1530) |
| `launcher\lang\*.ini` (13 files) | none (grep) | - |
| `gem_launcher.ini` | none (only the hotkey `actions/melee_toggle`, a mod action, line 38) | see B9 |
| `launcher\res\features.txt` (RCDATA 800, compiled in) | `[gem_meleestance]` (17-29), `[gem_meleechase]` (31-38); no `[gem_penfilter]` | the page lists only dlls in the build (L:2221-2226, 2297-2309); a dll without a section is listed at the end by file name (features.txt:6-7). gem_melee needs a `[gem_melee]` section in Guga's repo; the two old sections become dead text. |
| `launcher\res\features.html`, `launcher.rc`, `README.md` | README.md:84-85 table rows only (docs) | docs |
| `launcher\payload\` (worktree) | gem_meleechase.dll/.ini, gem_meleestance.dll/.ini (no gem_penfilter) | must be swapped for gem_melee.dll/.ini; `build.bat payload` of `_dlls\_indomitus_DLL_meleestance` / `_meleechase` copies the old files back in |
| `build_zig.bat`, `_tools\launcher_build\build_local.py` | none (generic `*.dll` / `*.ini`; build_zig.bat:86 only warns about gem_hostserver) | - |
| `_dlls\_indomitus_DLL_teleport` (gem_teleport.c / tp/teleport.c:504-509; gem_teleport.ini:58 comment) | `GetModuleHandleA("gem_meleestance.dll")` | section 3 / 4 |
| `_dlls\_indomitus_DLL_meleestance\src\gem_meleestance.c:1904` | `GetModuleHandleA("gem_teleport.dll")` | carried into gem_melee |
| `_dlls\_indomitus_DLL_contactdamage\build.bat:8`, usercontrol README:68, sensortags.c:38, squadfollow README:25,60, weaponscript README:52, teleport README:166 | comments / docs | none |
| `_tools\gem_{crewvoice,usercontrol,costred,sessiondump,teamtags,sensortags}\build.bat` | copy `gem_inject.exe` from `..\gem_meleestance\out` if it exists | tooling only (`if exist`, no failure) |
| MP host server path `<payload>\server\` | none: a full copy of every payload file (L:1442-1454); logs collected generically from `server*\*.log` (L:1319-1322) | gem_melee.dll + gem_melee.ini are copied with the rest; no server folder exists yet in a1d6ed3c or d6347bfc (never hosted with them) |
| Lobby check | gem_lobbycheck (not in a1d6ed3c) compares launcher build / payload fingerprint, not dll names (features.txt:233-240) | a new fingerprint for everyone, as with any payload change |
| Session log | launcher collects every `*.log` of the payload folder (L:1318) | gem_melee*.log picked up automatically |

---------------------------------------------------------------------------------------------------------------------

## 3. Cross-dll contracts (exact ABI)

Only two links exist between payload dlls and the three (grep of every payload source for GetModuleHandle /
GetProcAddress / module names; PE export tables via pe_info.py):

| Module | Exports (pe_info.py) |
|---|---|
| gem_meleestance.dll 0.7.41 (live) and 0.7.42 (dist) | `gem_quant_listen` only (explicit dllexport) |
| gem_teleport.dll 0.2.13 | `gem_order_listen` only |
| gem_meleechase.dll 0.5.4 | no dllexport: the linker auto-exported every global (mingw CRT symbols `_CRT_INIT`, `__xc_a`, ...); nobody imports them |
| gem_penfilter.dll 0.1.1 | auto-exported globals incl. `pf_stub_a1/a2/w/p/x`, `pf_solve`, `g_a2_back`, `g_vb`, ... (asm stubs reach them by symbol); nobody imports them |

### C1 `gem_meleestance.dll!gem_quant_listen` (provider: stance; consumer: Teleport)
- Provider (ms:1771-1794): `typedef void (__attribute__((ms_abi)) *quant_cb_t)(void);`
  `__declspec(dllexport) __attribute__((ms_abi)) void gem_quant_listen(quant_cb_t cb);`
  NULL ignored, duplicates ignored, up to 4 (`NQUANTCB`), lock-free `InterlockedCompareExchangePointer`, any thread,
  any time, no unregister; a 5th callback is dropped silently (no log line, ms:1782-1787).
- Calls: from `frame_hook` = the hooked eQuantExecutor slot, once per quant, on the engine thread that runs the
  quants, after stance's own `ms_tick()` and before the engine's run-quant (ms:1789-1794). Callbacks run even when
  `ms_tick` returns early (ClientGate, throttle). They only run once `frame_hook_try` has succeeded, which needs
  `[AutoDraw] Tick=1` (ms:1807) and is first tried from the scan hook (ms:1986-1987) or an order event (ms:1873); if
  stance's install stops early (Enabled=0 or a finder error, ms:3110-3112) neither entry exists and registered
  callbacks never run. Callbacks must not give orders (ms:1774).
- Consumer (tp/teleport.c:492-510): `tp_quant_register()` does `GetModuleHandleA("gem_meleestance.dll")` +
  `GetProcAddress(h, "gem_quant_listen")`, cast to `void (*)(void (*)(void))`, and passes `tp_quant` (default x64 ABI =
  ms_abi with this toolchain). Retried at most once per 1000 ms (GetTickCount64), with no end, from `tp_aim` (every
  IsApplicable of a `{teleport}` action, tp/teleport.c:514) and from the jump execution (tp/teleport.c:410); logs a
  failure at most twice ("aim: gem_meleestance.dll not loaded ..."). `tp_quant` sets `g_engineTid`, runs `tp_aim_scan`
  (fires `teleport_aim_begin` / `teleport_aim_end`) and `watch_tick` (tp/teleport.c:492-497).
- Without it: `tp_aim_scan` only runs from Teleport's GiveOrder hook (tp/teleport.c:568), so BOTH aim events (the
  jetpack range rings) fire only when some unit is given an order; the watch diagnostic does not tick.
- Live: no Teleport aim in the 12:16 run (no "aim:" line in log:gem_teleport); the earlier session in the same
  stance log registered (log:stance line 347 "quant listeners: callback ... registered (slot 1 of 4); live now").

### C2 `gem_teleport.dll!gem_order_listen` (provider: Teleport; consumer: stance)
- Provider (tp/teleport.c:543-562): `typedef void (*OrderListenFn)(void *entity, void *order);`
  `__declspec(dllexport) void gem_order_listen(OrderListenFn cb);` NULL / duplicates ignored, up to 4
  (`TP_MAXLISTEN`), lock-free CAS, any time ("list full" is logged). Callbacks are called from `hook_give_order` at
  its entry for every call with a non-NULL order, BEFORE the teleport lock decides, with `(entity, order object)`, on
  the thread that calls Entity::GiveOrder (the engine thread: scan hook, button clicks and GiveOrder share one thread,
  dll_hooks.md:170 0.7.23 entry), inside the order path; they must not give orders. Live only when the hook is installed:
  `lock_orders=1` (live ini line 64) and the finder passed (tp/main.c:87; tp/teleport.c:611-613).
- Consumer (ms:1863-1878, 1897-1908): `hb_thread` polls `GetModuleHandleA("gem_teleport.dll")` +
  `GetProcAddress(m, "gem_order_listen")` every 10th 100-ms sleep (~1 s) for the first 600 sleeps (~60 s), then
  logs "no gem_order_listen export ... order poll only" and stops trying. `hb_thread` is created only when
  `[MeleeStance] Log=1` AND install reached the end of the MatchAmmo patch (ms:3158). Callback `order_event(entity,
  order)`: returns at once inside stance's tick or hook (`g_in_tick || g_in_hook`), for non-humans and linked units;
  else sets `g_ui_tid`, calls `frame_hook_try()` (which is how the quant hook usually gets installed), counts, and
  queues the unit for the next poll. It gives no order.
- Live: log:stance line 472 "orders: listening to GEM Teleport's GiveOrder hook (gem_order_listen)"; log:gem_teleport
  12:16:04.756 "order listeners: callback 00007FF809623D50 registered (slot 1 of 4); live now" (0.3 s after Teleport
  loaded at 12:16:04.442).

### Consequences for gem_melee (facts, not design)
- Teleport 0.2.13 finds C1 only by the module base name `gem_meleestance.dll`. A module named gem_melee.dll alone =
  no per-quant tick for Teleport (see C1 "Without it").
- Stance's link to C2 hangs on `Log=1`, and C1's callbacks on stance's own install and on `[AutoDraw] Tick`.

---------------------------------------------------------------------------------------------------------------------

## 4. Teleport rebuild test (done)

- Copy: `inputs\teleport_0.2.13\` -> `C:\Users\alex_\OneDrive\Desktop\Claude_Indomitus_Project\_tools\gem_melee\compat\teleport_rebuild\` (unmodified).
- Command (build.bat:21-22 flags, zig from the rules instead of build.bat's search; no `payload` argument, so nothing
  copied anywhere), run in that folder:
  `C:\Users\alex_\Documents\IndomitusLauncher\zig\zig.exe cc -target x86_64-windows-gnu -O2 -s -shared -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -o dist\gem_teleport.dll src\gem_teleport.c -lkernel32 -luser32 -lm`
  (zig 0.14.0, exit 0, no warnings printed; `dist\*.lib` deleted as build.bat does).
- Result: **byte-identical**. SHA-256 of `compat\teleport_rebuild\dist\gem_teleport.dll` =
  `98D4F8FEB41403F21B93BFE34C4EB471B6B15A2C433DDE656F1BDBD488B3052D` = `inputs\live_payload_a1d6ed3c\gem_teleport.dll`
  (also = MANIFEST.json, the worktree `launcher\payload` copy and Guga's d6347bfc copy). Size 65024 both, TimeDateStamp
  74CA08F2 both (zig writes a content-derived stamp), same 7 sections, same single export `gem_order_listen`.
- Note: build.bat has no `-mcpu=baseline -ffp-contract=off -fno-fast-math`; the identical result was reached without them.
- So the plan's first branch is open: a Teleport 0.2.14 that looks for `gem_melee.dll` first, then `gem_meleestance.dll`,
  can be built from Guga's source and its diff checked against 0.2.13. Teleport is Guga's dll
  (`_dlls\_indomitus_DLL_teleport` in his repo).

---------------------------------------------------------------------------------------------------------------------

## 5. Early injection: facts the merged DllMain must respect

### 5.1 How and when (launcher)
- `CreateProcessW(..., CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT, ...)` (L:1786-1788); `InjectEarlyBird` queues
  one `QueueUserAPC(LoadLibraryW, remote path)` per dll on the main thread (L:1097-1136); `ResumeThread` (L:1802).
  Comment L:9-10: the APCs "run during loader init, before the game's entry point" (= ntdll delivers them at the end of
  process initialisation, before the exe entry point; standard Windows behaviour, inferred).
- Each LoadLibraryW runs one DllMain under the loader lock; the next APC starts after it returns (sequential:
  log timestamps fauna 12:16:02.295 ... chase 03.178-03.458 ... teleport 04.442).
- The launcher then polls for every dll by base name for up to 5 s and **kills the game** if one is missing
  (L:1806-1839; `IsModuleLoaded` compares `GetModuleBaseNameW`, L:1138-1150). A DllMain returning FALSE or crashing =
  no game. The three always return TRUE (ms:3283-3303, mc/main.c:122-131, pf:1933-1954).
- State of the exe at that point:
  - Mapped, relocated, imports resolved, not packed: the exes have no packer / SteamStub section (`.text .rdata .data
    .pdata .gem .rsrc .reloc`, pe_info.py), and the dlls' DllMain finders matched .text byte patterns at 12:16:02-03
    (e.g. chase "eVS_Root tick exe+0x82F700" at 12:16:03.253, which vt_slots.py finds in the file on disk).
  - The exe's own static initialisers / CRT / WinMain have NOT run: Teleport's instruction registry and ScriptExt's
    condition registry are empty at DllMain and are filled "from its static initialisers (on the main thread, while
    this thread already runs)" (tp/main.c:10-13); logs: Teleport "waiting for the instruction registry to
    initialise" 12:16:04.602 -> "registered" 12:16:05.283, ScriptExt the same. No engine threads, no scene, no sim
    clock value, no entities yet.
- MP server way 2 (L:1543-1576): `CreateRemoteThread(LoadLibraryW)` into a server that is already running: DllMain on
  a new thread under the loader lock while engine threads execute. Stance and Pen patch with plain `memcpy` after
  `VirtualProtect` without stopping other threads (ms:3141-3154, 3193-3198, 3214-3218; pf:1743-1751); Chase stops every
  other thread around each code write (mc/patch.c:37-77); NoProne / SquadFollow also handle a running game (gem_noprone.c:395).

### 5.2 What the three do in DllMain today
| | MeleeStance 0.7.42 | MeleeChase 0.5.4 | PenFilter 0.1.1 |
|---|---|---|---|
| Entry | ms:3283-3303 | mc/main.c:122-131 -> `init` mc/main.c:16-120 | pf:1933-1954 |
| Thread notifications | DisableThreadLibraryCalls | DisableThreadLibraryCalls | DisableThreadLibraryCalls |
| Exe gate | none | `_wcsnicmp(exe, "call_to_arms", 12)` else nothing (mc/main.c:23) | none |
| ini / log names | `<dll path minus .dll>.ini`; log `<same>.log` / `<same>_editor.log` / `<same>_server.log` by `strstr(exe,"_ed")` / `"_server"`, append mode, opened on first line (ms:86-98, 3288-3298) | fixed `<dll dir>\gem_meleechase.ini` / `\gem_meleechase.log` (gem_meleechase.c:45-46), log truncated ("w") at start, no editor / server suffix (mc/main.c:26) | like stance (pf:1938-1948), append mode (pf:101-105) |
| Finders | all, scanning the whole .text, `.pdata`, RTTI (ms:2840-2944 + find_charge / find_instant / find_takeitem / find_tagcont) | all (mc/main.c:69-118) | all (pf:405-640) |
| Code / data writes in DllMain | MatchAmmo prologue 14 bytes -> `jmp [rip]` + trampoline in a VirtualAlloc page (ms:3141-3154); `[ShieldMelee]` ray site 30 bytes only if Enabled (0 today, ms:3177-3204); ExecuteInstant slot in 9 vtables (ms:3205-3226); blow table 12 bytes in .rdata (ms:3268-3280) | VEH (mc/main.c:31); IAT exit hooks (mc/crashlog.c:77-89); eMoveTask slots 0 / 19, eVS_Root slot 3, approach call, eval tail jmp, 4 SetTarget calls, all via thread-stopping `patch_code` / `vt_hook` (mc/melee.c:1045-1061, mc/patch.c) ; `InitializeCriticalSection` | builder prologue 15 bytes, A1 5, A2 8, B 5, P `PICKP_LEN`, X 5 bytes, with jump pads from `alloc_near` (pf:1786-1904) |
| Deferred to run time | quant executor slot (`frame_hook_try`, engine thread's own stack walk, ms:1805-1862) | entity tag ids built lazily (brief:667) | - |
| Threads started in DllMain | `hb_thread` only when Log=1 and install got that far (ms:3158) | none (`AllocConsole` only if `console=1`) | `stats_thread` when Log=1 and A, B or P is on (pf:1951) |

### 5.3 Unsafe or impossible inside the merged DllMain (from the facts above)
- Calling engine code that needs the exe's static state (registries, scene, entities, sim clock value): not built yet.
- Waiting for a thread it created (loader-lock deadlock; threads made in DllMain start running only once the lock is
  free - standard Windows behaviour, inferred; the stance / pen / teleport helper threads only Sleep and poll).
- Finding the quant executor slot: stance needs the ENGINE thread's stack (ms:1816-1827); the APC runs on the main
  thread before any engine thread exists. Must stay lazy.
- Looking up `gem_teleport.dll`: it loads 13 dlls later, so it must be a later poll (stance's hb_thread) or a
  `LdrRegisterDllNotification`-style callback (not used by any payload dll today).
- Seeing old gem_meleestance / gem_meleechase / gem_penfilter: they load after gem_melee (2.2).
- Returning FALSE / throwing: the launcher kills the process (5.1).
- In the server way-2 path the code writes race running engine threads (5.1); only chase's writes are protected today.

---------------------------------------------------------------------------------------------------------------------

## 6. The three inis as shipped (three-way, generated)

Columns: code default (snapshot source line), snapshot ini(s), live a1d6ed3c ini. GetPrivateProfile rules applied
(first section / key wins, case-insensitive, no inline-comment stripping). "INIs DIFFER" = the effective values
differ between the columns; "ini != code" = an ini sets a value other than the code default.

Summary of the differences
| dll | key | code | snapshot ini | live a1d6ed3c | value Alex has been testing | note |
|---|---|---|---|---|---|---|
| stance | `[AutoDraw] ProbeAnyRange` | 1 (ms:3043) | 1 (src and dist) | **0** | **0** (every payload since Guga's 0.9.15 commit bf6b01c, incl. d6347bfc and a1d6ed3c; brief:211) | the merged ini must say 0 |
| stance | `[Charge] CooldownMs` | 4000 (ms:2991) | 2000 | 2000 | 2000 | keep the key in the ini (code default differs) |
| stance | `[MeleeStance] AutoMeleeTag` | `melee_infantry` (ms:3032, new in 0.7.42) | `melee_infantry` | absent (0.7.41 ini; 0.7.41 has no such rule) | none: 0.7.42 never ran; Alex chose the rule (dll_hooks.md:179 at the time of writing, melee_dll_merge_brief.md:698) | with the 0.7.41 ini the 0.7.42 code default turns the rule ON anyway; write it explicitly |
| chase | (none) | | = live (same SHA-256 6cc50015...) | | live values | `repath_min_m=1`, `repath_max_m=10`, `close_range_m=10`, `lead_max_m=6` are written as integers, equal to the code's 1.0 / 10.0 / 10.0 / 6.0 |
| pen | (none) | | = live (same SHA-256 86558037...) | | live values | `BlockStreak` read by code (-1, ignored since 0.1.1, pf:1689, 1895) but in no ini |

Other facts for the merged ini
- Section names of the three do not collide: stance `[MeleeStance] [Charge] [Zerg] [MultiSelect] [ShieldMelee]
  [Downed] [BlowReach] [AutoDraw] [Vehicles] [ReloadCancel]`, chase `[meleechase]`, pen `[PenFilter]`. Same key names
  in different sections (Enabled, Log, PlayerUnits, SkipDirectControl) stay separate.
- The stance src ini (19867 bytes, 12:58) and dist ini (19182 bytes, 12:39) give identical values; they differ in
  comments only. The live 0.7.41 ini is 17719 bytes.
- No ini key of any of the three is unread by its code.

### MeleeStance 0.7.42


| section | key | code default (src line) | snapshot src ini | snapshot dist ini (12:39) | live a1d6ed3c ini (0.7.41) | diff |
|---|---|---|---|---|---|---|
| MeleeStance | Enabled | `1` (2964) | `1` | `1` | `1` |  |
| MeleeStance | Log | `1` (2965) | `1` | `1` | `1` |  |
| MeleeStance | SkipLinked | `1` (2966) | `1` | `1` | `1` |  |
| MeleeStance | DrawFromBelt | `1` (2967) | `1` | `1` | `1` |  |
| MeleeStance | SkipDirectControl | `1` (2968) | `1` | `1` | `1` |  |
| AutoDraw | OrderPicks | `1` (2973) | `1` | `1` | `1` |  |
| MeleeStance | DiagManual | `0` (2974) | `0` | `0` | `0` |  |
| MeleeStance | HookTiming | `1` (2975) | `1` | `1` | `1` |  |
| MeleeStance | FastPass | `1` (2976) | `1` | `1` | `1` |  |
| MeleeStance | ClientGate | `1` (2977) | `1` | `1` | `1` |  |
| Charge | Enabled | `1` (2982) | `1` | `1` | `1` |  |
| Charge | MeleeInHand | `1` (2984) | `1` | `1` | `1` |  |
| Charge | PlayerUnits | `1` (2985) | `1` | `1` | `1` |  |
| Zerg | Enabled | `1` (2986) | `1` | `1` | `1` |  |
| Zerg | Distance | `1600` (2987) | `1600` | `1600` | `1600` |  |
| Zerg | ReorderMs | `700` (2988) | `700` | `700` | `700` |  |
| Charge | CooldownMs | `4000` (2991) | `2000` | `2000` | `2000` | **ini != code** |
| Charge | RetargetMs | `1500` (2992) | `1500` | `1500` | `1500` |  |
| Charge | RefreshMs | `400` (2993) | `400` | `400` | `400` |  |
| Charge | RunMode | `2` (3002) | `2` | `2` | `2` |  |
| MultiSelect | Enabled | `1` (3014) | `1` | `1` | `1` |  |
| MeleeStance | DrawOnToggle | `1` (3015) | `1` | `1` | `1` |  |
| MeleeStance | DrawCooldownMs | `3000` (3016) | `3000` | `3000` | `3000` |  |
| ShieldMelee | Enabled | `0` (3018) | `0` | `0` | `0` |  |
| Downed | Enabled | `1` (3021) | `1` | `1` | `1` |  |
| BlowReach | Enabled | `1` (3024) | `1` | `1` | `1` |  |
| AutoDraw | Enabled | `1` (3033) | `1` | `1` | `1` |  |
| AutoDraw | PlayerUnits | `0` (3034) | `0` | `0` | `0` |  |
| AutoDraw | HoldMs | `3000` (3035) | `3000` | `3000` | `3000` |  |
| AutoDraw | Probe | `1` (3039) | `1` | `1` | `1` |  |
| AutoDraw | ProbeMs | `300` (3040) | `300` | `300` | `300` |  |
| AutoDraw | ProbeCooldownMs | `4000` (3041) | `4000` | `4000` | `4000` |  |
| AutoDraw | NoMeleeMs | `20000` (3042) | `20000` | `20000` | `20000` |  |
| AutoDraw | ProbeAnyRange | `1` (3043) | `1` | `1` | `0` | **INIs DIFFER, ini != code** |
| AutoDraw | KnownMs | `300000` (3044) | `300000` | `300000` | `300000` |  |
| AutoDraw | OrderPoll | `1` (3045) | `1` | `1` | `1` |  |
| AutoDraw | PollMs | `250` (3046) | `250` | `250` | `250` |  |
| AutoDraw | BlindCheckMs | `8000` (3047) | `8000` | `8000` | `8000` |  |
| AutoDraw | Proximity | `1` (3048) | `1` | `1` | `1` |  |
| AutoDraw | EscalateMs | `1500` (3049) | `1500` | `1500` | `1500` |  |
| AutoDraw | Tick | `1` (3051) | `1` | `1` | `1` |  |
| AutoDraw | TickMs | `100` (3052) | `100` | `100` | `100` |  |
| AutoDraw | FightOrder | `1` (3054) | `1` | `1` | `1` |  |
| AutoDraw | FightCooldownMs | `1500` (3055) | `1500` | `1500` | `1500` |  |
| Vehicles | Enabled | `1` (3057) | `1` | `1` | `1` |  |
| Vehicles | InfantryMs | `500` (3058) | `500` | `500` | `500` |  |
| Vehicles | CloseAttackOrder | `1` (3063) | `1` | `1` | `1` |  |
| Vehicles | AdjacentMs | `1500` (3066) | `1500` | `1500` | `1500` |  |
| ReloadCancel | Enabled | `1` (3071) | `1` | `1` | `1` |  |
| MeleeStance | SkipTags | `tp_jetpack,hidden,melee_disabled,disable_melee,valkyrie,sensor_off` (2969) | `tp_jetpack,hidden,melee_disabled,disable_melee,valkyrie,sensor_off` | `tp_jetpack,hidden,melee_disabled,disable_melee,valkyrie,sensor_off` | `tp_jetpack,hidden,melee_disabled,disable_melee,valkyrie,sensor_off` |  |
| MeleeStance | IgnoreTargetTags | `hidden,melee_disabled,valkyrie,sensor_off` (2971) | `hidden,melee_disabled,valkyrie,sensor_off` | `hidden,melee_disabled,valkyrie,sensor_off` | `hidden,melee_disabled,valkyrie,sensor_off` |  |
| MeleeStance | Tag | `melee_stance` (2978) | `melee_stance` | `melee_stance` | `melee_stance` |  |
| MeleeStance | MeleeAmmoTags | `melee_light,melee_heavy,medium_inf_melee_target,medium_veh_melee_target,heavy_veh_melee_target` (2979) | `melee_light,melee_heavy,medium_inf_melee_target,medium_veh_melee_target,heavy_veh_melee_target` | `melee_light,melee_heavy,medium_inf_melee_target,medium_veh_melee_target,heavy_veh_melee_target` | `melee_light,melee_heavy,medium_inf_melee_target,medium_veh_melee_target,heavy_veh_melee_target` |  |
| Charge | Tag | `has_melee_weapon` (2983) | `has_melee_weapon` | `has_melee_weapon` | `has_melee_weapon` |  |
| Zerg | Tag | `melee_infantry` (2989) | `melee_infantry` | `melee_infantry` | `melee_infantry` |  |
| Charge | SwitchRatio | `0.6` (2996) | `0.6` | `0.6` | `0.6` |  |
| Charge | ProgressUnits | `10` (2998) | `10` | `10` | `10` |  |
| Charge | CloseDistance | `200` (3000) | `200` | `200` | `200` |  |
| Charge | MoveRefreshUnits | `40` (3003) | `40` | `40` | `40` |  |
| Charge | MoveRefreshFraction | `0.4` (3005) | `0.4` | `0.4` | `0.4` |  |
| Charge | MaxDistance | `2000` (3008) | `2000` | `2000` | `2000` |  |
| Charge | PlayerMaxDistance | `1000` (3010) | `1000` | `1000` | `1000` |  |
| Charge | MinDistance | `60` (3012) | `60` | `60` | `60` |  |
| MeleeStance | DrawButton | `melee_toggle` (3017) | `melee_toggle` | `melee_toggle` | `melee_toggle` |  |
| ShieldMelee | SeeThroughTag | `forcefield` (3019) | `forcefield` | `forcefield` | `forcefield` |  |
| Downed | Tag | `start_personage` (3022) | `start_personage` | `start_personage` | `start_personage` |  |
| MeleeStance | AlwaysTag | `melee_always` (3031) | `melee_always` | `melee_always` | `melee_always` |  |
| MeleeStance | AutoMeleeTag | `melee_infantry` (3032) | `melee_infantry` | `melee_infantry` | (absent: code default) |  |
| AutoDraw | Distance | `100` (3036) | `100` | `100` | `100` |  |
| AutoDraw | Item | `melee` (3038) | `melee` | `melee` | `melee` |  |
| AutoDraw | LeaveFactor | `1.5` (3050) | `1.5` | `1.5` | `1.5` |  |
| Vehicles | InfantryRange | `400` (3059) | `400` | `400` | `400` |  |
| Vehicles | CloseDistance | `400` (3061) | `400` | `400` | `400` |  |
| Vehicles | VehicleMeleeTags | `melee_light,melee_heavy` (3064) | `melee_light,melee_heavy` | `melee_light,melee_heavy` | `melee_light,melee_heavy` |  |
| Vehicles | ContactTag | `hull_contact` (3067) | `hull_contact` | `hull_contact` | `hull_contact` |  |
| Vehicles | AdjacentMax | `120` (3069) | `120` | `120` | `120` |  |
| ReloadCancel | KeepTags | `mgun,hmg_shell,stubber,lascannon,plasmacannon,sm_weapon_plasmacannon,sm_weapon_heavybolter,rocketlauncher,heavy_flamethrower,grenadelauncher,at_rifle,ogryn_weapon` (3072) | `mgun,hmg_shell,stubber,lascannon,plasmacannon,sm_weapon_plasmacannon,sm_weapon_heavybolter,rocketlauncher,heavy_flamethrower,grenadelauncher,at_rifle,ogryn_weapon` | `mgun,hmg_shell,stubber,lascannon,plasmacannon,sm_weapon_plasmacannon,sm_weapon_heavybolter,rocketlauncher,heavy_flamethrower,grenadelauncher,at_rifle,ogryn_weapon` | `mgun,hmg_shell,stubber,lascannon,plasmacannon,sm_weapon_plasmacannon,sm_weapon_heavybolter,rocketlauncher,heavy_flamethrower,grenadelauncher,at_rifle,ogryn_weapon` |  |
| BlowReach | Near | `45` (3025) | `45` | `45` | `45` |  |
| BlowReach | Medium | `45` (3025) | `45` | `45` | `45` |  |
| BlowReach | Far | `45` (3025) | `45` | `45` | `45` |  |

- every key in these inis is read by the code

### MeleeChase 0.5.4


| section | key | code default (src line) | snapshot ini | live a1d6ed3c ini | diff |
|---|---|---|---|---|---|
| meleechase | enabled | `1` (24) | `1` | `1` |  |
| meleechase | log | `1` (25) | `1` | `1` |  |
| meleechase | console | `0` (25) | `0` | `0` |  |
| meleechase | verbose | `1` (25) | `1` | `1` |  |
| meleechase | crash_log | `1` (30) | `1` | `1` |  |
| meleechase | vehicle_chase | `1` (42) | `1` | `1` |  |
| meleechase | repath_interval_ms | `500` (47) | `500` | `500` |  |
| meleechase | repath_interval_close_ms | `200` (49) | `200` | `200` |  |
| meleechase | pursue_retreating | `1` (55) | `1` | `1` |  |
| meleechase | chase_always | `1` (56) | `1` | `1` |  |
| meleechase | chase_run_mode | `2` (57) | `2` | `2` |  |
| meleechase | hold_ranged_in_melee | `1` (58) | `1` | `1` |  |
| meleechase | smooth_repath | `1` (62) | `1` | `1` |  |
| meleechase | log_chase | `1` (63) | `1` | `1` |  |
| meleechase | weapon_priority | `1` (64) | `1` | `1` |  |
| meleechase | melee_tag | `melee` (35) | `melee` | `melee` |  |
| meleechase | vehicle_melee_tag | `veh_melee` (39) | `veh_melee` | `veh_melee` |  |
| meleechase | prop_prioritize_melee | `ai_prioritize_melee` (65) | `ai_prioritize_melee` | `ai_prioritize_melee` |  |
| meleechase | prop_prioritize_ranged | `ai_prioritize_ranged` (66) | `ai_prioritize_ranged` | `ai_prioritize_ranged` |  |
| meleechase | repath_min_m | `1.0 (clamp 0.2..100.0)` (43) | `1` | `1` |  |
| meleechase | repath_max_m | `10.0 (clamp 0.2..500.0)` (44) | `10` | `10` |  |
| meleechase | repath_fraction | `0.25 (clamp 0.0..1.0)` (46) | `0.25` | `0.25` |  |
| meleechase | close_range_m | `10.0 (clamp 0.0..200.0)` (51) | `10` | `10` |  |
| meleechase | contact_factor | `0.5 (clamp 0.1..1.0)` (52) | `0.5` | `0.5` |  |
| meleechase | lead_seconds | `0.6 (clamp 0.0..5.0)` (53) | `0.6` | `0.6` |  |
| meleechase | lead_max_m | `6.0 (clamp 0.0..50.0)` (54) | `6` | `6` |  |
| meleechase | hold_ranged_reach_factor | `1.0 (clamp 0.2..5.0)` (59) | `1.0` | `1.0` |  |
| meleechase | retreat_speed_mps | `0.3 (clamp 0.0..20.0)` (61) | `0.3` | `0.3` |  |

- every key in these inis is read by the code

### PenFilter 0.1.1


| section | key | code default (src line) | snapshot ini | live a1d6ed3c ini | diff |
|---|---|---|---|---|---|
| PenFilter | Enabled | `1` (1672) | `1` | `1` |  |
| PenFilter | Log | `1` (1673) | `1` | `1` |  |
| PenFilter | LogVerdicts | `400` (1674) | `400` | `400` |  |
| PenFilter | HookRowFilter | `1` (1675) | `1` | `1` |  |
| PenFilter | HookAimCheck | `1` (1676) | `1` | `1` |  |
| PenFilter | HookAimPriority | `1` (1677) | `1` | `1` |  |
| PenFilter | HookExhaust | `1` (1678) | `1` | `1` |  |
| PenFilter | ExhaustStop | `1` (1679) | `1` | `1` |  |
| PenFilter | HookApproach | `0` (1680) | `0` | `0` |  |
| PenFilter | CacheMs | `60000` (1681) | `60000` | `60000` |  |
| PenFilter | VerdictHoldMs | `8000` (1682) | `8000` | `8000` |  |
| PenFilter | HoldDistanceM | `15` (1683) | `15` | `15` |  |
| PenFilter | UnreachableMs | `3000` (1684) | `3000` | `3000` |  |
| PenFilter | PierceRule | `1` (1685) | `1` | `1` |  |
| PenFilter | PlayerUnits | `1` (1686) | `1` | `1` |  |
| PenFilter | SkipDirectControl | `1` (1687) | `1` | `1` |  |
| PenFilter | Facing | `1` (1688) | `1` | `1` |  |
| PenFilter | BlockStreak | `-1` (1689) | (absent: code default) | (absent: code default) |  |
| PenFilter | PierceTag | `pierce` (1695) | `pierce` | `pierce` |  |
| PenFilter | ExemptTags | `melee_light,melee_heavy,melee` (1697) | `melee_light,melee_heavy,melee` | `melee_light,melee_heavy,melee` |  |
| PenFilter | AimPriority | `body,cabin,bodyback;turret,mantlet;aux_turret1,aux_turret2,aux_turret3,aux_turret4,aux_turret5,aux_turret6,aux_turret7,aux_turret8,aux_turret9;arm_left,arm_right,gun_left,gun_right,hkm_left,hkm_right;leg_left,leg_right,frontleftleg,frontrightleg,midleftleg,midrightleg,backleftleg,backrightleg;mover,track,trackleft,trackright,wheel` (1701) | `body,cabin,bodyback;turret,mantlet;aux_turret1,aux_turret2,aux_turret3,aux_turret4,aux_turret5,aux_turret6,aux_turret7,aux_turret8,aux_turret9;arm_left,arm_right,gun_left,gun_right,hkm_left,hkm_right;leg_left,leg_right,frontleftleg,frontrightleg,midleftleg,midrightleg,backleftleg,backrightleg;mover,track,trackleft,trackright,wheel` | `body,cabin,bodyback;turret,mantlet;aux_turret1,aux_turret2,aux_turret3,aux_turret4,aux_turret5,aux_turret6,aux_turret7,aux_turret8,aux_turret9;arm_left,arm_right,gun_left,gun_right,hkm_left,hkm_right;leg_left,leg_right,frontleftleg,frontrightleg,midleftleg,midrightleg,backleftleg,backrightleg;mover,track,trackleft,trackright,wheel` |  |

- every key in these inis is read by the code

---------------------------------------------------------------------------------------------------------------------

## 7. Hazards and open points (summary; details in the sections)
See the orchestrator data returned with this map (hazards / questions). The main ones:
1. Teleport's per-quant tick (C1) is bound to the module name gem_meleestance.dll, to stance's own install succeeding,
   and to `[AutoDraw] Tick=1`; without it the jetpack aim events only fire on orders.
2. Stance's Teleport link (C2) is created only with `[MeleeStance] Log=1`.
3. gem_melee must never inline-hook Entity::GiveOrder exe+0x67E750 (S1) and must keep loading after Fauna and before
   Teleport / UserControl.
4. A DllMain guard cannot see old dlls that load later; Teleport 0.2.13 would bind to an inert old gem_meleestance.dll.
5. Stance and pen write code without stopping threads; harmless early-bird, a race in server way 2.
6. `[AutoDraw] ProbeAnyRange=0` and `[Charge] CooldownMs=2000` must be carried; `AutoMeleeTag` must be written explicitly.
