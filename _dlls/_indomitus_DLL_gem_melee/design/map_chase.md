# map_chase.md: GEM MeleeChase 0.5.4 source map (for the gem_melee merge)

Snapshot: `inputs\meleechase_0.5.4\` (paths below are relative to it). `S` = `src\gem_meleechase.c`, `core` = `src\mc\core.c`,
`patch` = `src\mc\patch.c`, `melee` = `src\mc\melee.c`, `crash` = `src\mc\crashlog.c`, `main` = `src\mc\main.c`,
`test` = `src\mc\test_main.c`. Line numbers are the snapshot's. "inferred" = not proven by code I read.

This file was written in reading order first (raw notes per file, sections R1..R7 at the end), then the 10 numbered
sections required by the brief were filled from those notes. The numbered sections come first.

---------------------------------------------------------------------------------------------------------------------

<!-- numbered sections begin -->
## 0. Summary (read this first)
- One unity translation unit (`S` includes core, patch, melee, crashlog, main, test_main). Everything is `static`
  except `DllMain` (main:122) and, in the FX_TEST build only, `main` (test:7). No `__asm__`, no exports of its own, no
  imports from other GEM dlls, no threads. Everything (finders + patches) runs inside DllMain on the injecting thread.
- Engine patches: 3 vtable slot swaps (eVS_Root slot 3, eMoveTask slots 0 and 19) and 6 call-site rel32 redirects (the
  approach call, the target evaluation's single tail jmp, 4 Weapon::SetTarget calls). No prologue detour, no inline
  byte patch, no data table write. Plus the crash log: a vectored handler and IAT hooks on ExitProcess /
  TerminateProcess (0 entries hooked in today's payload because GEM Fauna, loaded earlier, already hooked them).
- Every finder was run on the installed 1.069 exes with a Windows port of the module's own test (`probe\chase\`):
  ALL FOUND on game 6AFC6D57, editor 6AFC6E14, server 6AFC6DAF; editor values equal the live log of payload a1d6ed3c.
  None of chase's finder windows or whole-.text hits overlaps the 7 known MeleeStance / PenFilter code patch sites on
  any exe, and no MS / PF finder reads a byte chase patches (section 3), so the install order is free for the known
  sites; keep chase first anyway (today's order).
- Rebuilding the snapshot with build.bat's flags reproduces the live dll byte for byte (sha256 e992118a...). The merged
  flags change code only through `-ffp-contract=off` (SLP-vectorised x/y float math in 5 places); results are
  bit-identical (section 6b).
- Decisions use only the simulation clock and simulation state. Wall clock = log time stamps only.

## 1. Start-up (DllMain to the last installer call)
Injection: the launcher queues `LoadLibraryW(<payload>\gem_meleechase.dll)` as an APC on the suspended main thread of
the game (`Documents\IndomitusLauncher_mc054\launcher\main.cpp:9-12, 1121-1133`; the dedicated server gets its own copy in `<payload>\server\`,
main.cpp:1380-1382), so DllMain runs on the game's first thread before the exe's own start-up code (inferred from APC
timing). Payload dlls load in file-name order: gem_fauna (hooks the exit IATs, patches calls near the vehicle tick)
loads before gem_meleechase, MeleeStance and PenFilter after.

| step | code | gate / effect when it fails |
|---|---|---|
| DllMain PROCESS_ATTACH: DisableThreadLibraryCalls, init(h) | main:124-126 | returns TRUE always |
| g_self, g_dir = folder of this dll | main:18-20 | - |
| exe gate: file name must start with `call_to_arms` (12 chars, case-insensitive) | main:21-23 | silent return, no log file (also lets `call_to_arms_workshop.exe` through) |
| `enabled` (1) | main:24 | 0 = silent return |
| `log` (1), `console` (0), `verbose` (1, unused anywhere); log `_wfopen(<dir>\gem_meleechase.log, "w")` | main:25-27 | - |
| get_sections (exe `.text/.rdata/.data` required, `.pdata` optional) | main:28, core:56-71 | `not a PE I understand`, return |
| banner `GEM MeleeChase 0.5.4 loaded into %ls (timestamp %08X)` | main:29 | - |
| `crash_log` (1): AddVectoredExceptionHandler(1, fx_veh), install_exit_hooks | main:30-33 | - |
| all ini settings read (section 6 table) | main:35-67 | `melee_tag` empty: `melee_tag is empty - nothing to do`, return |
| find_weapon_layout | main:69-70 | fail = WHOLE DLL OFF (`melee weapon test OFF ... nothing to do`) |
| `humans not touched ...` line | main:75 | - |
| `vehicle_chase` (1) | main:78 | 0 = vehicle part off (ini) |
| find_weapons (always runs: `!o_wlistBeg` is always true here) | main:79 | fail = vehicle part OFF (weapon list not found) |
| found = find_vehicle | main:81 | fail = chase, close combat, priority, hold all off |
| prio = found && find_priority; g_okClose = prio; g_okPriority = prio && weapon_priority | main:85-87 | fail = close combat + priority + hold off; the plain re-plan still runs (unit_reach then returns -1 for melee units: no contact radius / lead, reach = goal radius) |
| prioTags = g_okPriority && find_entity_tags | main:89 | fail = def props only (o_entTags = 0) |
| if found && chase_run_mode >= 0 && !find_run_mode: o_modeSlot = 0 | main:90 | fail = no SetMovementMode call |
| g_okVehicle = found && install_vehicle() | main:91, melee:1053-1061 | fail: g_okPriority = g_okClose = 0; hooks already written stay (no rollback, they pass through) |
| status lines | main:93-99 | - |
| if g_okVehicle && g_okClose && hold_ranged_in_melee: g_okHold = find_hold() && install_hold() | main:100-103, melee:1045-1051 | fail = hold off (`OFF (engine code not recognised)`); partial: eval redirect stays if SetTarget site 0 fails; sites after a failed one are not patched but install returns 1 |
| run mode / priority status lines | main:104-117 | - |
| `simulation change: every player of a multiplayer match needs this DLL` | main:119 | - |

Install order inside install_vehicle: InitializeCriticalSection(&g_cs); vt eMoveTask[0] = mt_dtor; vt eMoveTask[19] =
mt_update; redirect approach call -> ap_move; vt eVS_Root[3] = vs_tick (last, so nothing registers before the order
hooks exist). install_hold: redirect the evaluation's tail jmp -> eval_wrap first, then each SetTarget site -> st_wrap
(so st_wrap never runs without eval_wrap having set its TLS brain).
Threads started: none (grep: no CreateThread / _beginthread). DLL_PROCESS_DETACH with reserved == NULL logs
`DLL detach (FreeLibrary)`; nothing is unhooked.

## 2. Hook inventory
Addresses = exe offsets (RVA) from this map's Windows port of the module's offline test (`probe\chase\out_*.txt`);
editor values also equal the live log `inputs\live_payload_a1d6ed3c\gem_meleechase.log` lines 13-21. README's
addresses are 1.067 game (stale).

| id | engine site, what the engine does there | game 6AFC6D57 | editor 6AFC6E14 | server 6AFC6DAF | kind | expected original / verification | handler | when / how often | reads / writes in engine memory | returns | locks, threads |
|---|---|---|---|---|---|---|---|---|---|---|---|
| V1 | `Actor::eVS_Root` vtable slot 3 = the vehicle brain's root-state tick (runs only when the brain has no AI order and no user order, README:193-194; contains the "approach" behaviour) | slot 0x1116900 -> fn 0x9AEDA0 | slot 0xF76E28 -> fn 0x82F700 | slot 0xE15270 -> fn 0x7C1120 | vtable slot swap (8-byte store, .rdata made RW) | finder: slot value in .text + tick head pattern (melee:369-382). vt_hook re-checks nothing; stores whatever is there as orig (chains if another dll swapped first) | `vs_tick` (melee:852-925), C, no stub | every brain update of every vehicle-brain unit whose root state runs; live log shows one unit logging ~6 ms apart | R: entity flags +0x30, weapon list, item tags, weapon range (engine call per weapon), entity tags +0x4E8 / def props, brain mode +0x1E0, records +0x3A0, record pos/entity, entity pos +0x74, sim clock. W: brain+0x3D4 approach range (apply_priority, chase_always / pursue), TLS t_*, g_tr, g_ch; calls chassis SetMovementMode (vtable +0x168) on registration | original tick's value | g_cs around track() and the registration; no engine call under the lock; TLS for the hand-off to ap_move |
| V2 | approach call in the tick: `eMoveTask(circle{x,y,r})` builder (r = 0.95 x weapon range) | site 0x9AF497 `E8 74 B5 F5 FF` -> 0x90AA10 | site 0x82FE14 `E8 37 8C F5 FF` -> 0x788A50 | site 0x7C1817 `E8 D4 B1 F5 FF` -> 0x71C9F0 | call-site rel32 redirect to a near pad `FF 25 00000000 <abs64 ap_move>` (pad = own 64 KB granule from alloc_near) | finder: E8 within 0xA0 after `movabs rax,"approach"` (same function by .pdata), target in .text whose first 0x40 bytes `lea rax,[eMoveTask vtable]` (melee:384-397). Patch time: only the 4 rel32 bytes are written, opcode / old rel32 not re-checked (patch:108-114) | `ap_move` (melee:743-755) | each approach order a vehicle brain gives itself (rare: per approach, live log ~every 20-25 ms for one stuck unit) | W: the engine's circle (radius -> max(reach x contact, 10), centre -> lead point) BEFORE calling the original; TLS t_apOrder/X/Y/R | the original builder's order pointer | none; TLS |
| V3 | `Order::eMoveTask` vtable slot 19 = Update(order, phase 0 start / 1 tick / 2 stop) | slot 0x1023A08 -> 0x90E3F0 | slot 0xEA1778 -> 0x78BC80 | slot 0xD5F168 -> 0x7203D0 | vtable slot swap | finder: `85 D2 74 xx 83 EA 01` in first 0x40 (melee:413-416) | `mt_update` (melee:927-994) | EVERY eMoveTask of EVERY unit (player moves, AI moves, MeleeStance's far-charge MakeMove orders), every tick while it runs; fast path `g_chLive == 0` = straight to original without lock | R: chase slot, brain mode / records, entity pos, sim clock. W: g_ch (next, repath, remove), g_tr | original's value; or 1 (phase 1: target in reach and not receding, or goal moved > threshold: order ends); or 1 after `FinishSub(order)` on a smooth stop (phase 2 without the chassis stop) | g_cs (linear search <= 512) on every update while any chase is live; lock released before any engine call |
| V4 | `Order::eMoveTask` vtable slot 0 = scalar deleting dtor | slot 0x1023970 -> 0xBB490 | slot 0xEA16E0 -> 0xAB720 | slot 0xD5F0D0 -> 0x8F900 | vtable slot swap | finder: slot 0 in .text and `F6 C3 01` in first 0x20 (melee:411) | `mt_dtor` (melee:998-1006) | every eMoveTask deletion | W: g_ch remove | original | g_cs only when g_chLive > 0 |
| H1 | the single jump into the vehicle target evaluation (resets approach range to -1, picks a weapon per slot/target, hands targets out with SetTarget; README:210-213) | 0x9A76A7 `E9 F4 08 00 00` -> 0x9A7FA0 (caller fn 0x9A74E0) | 0x827927 `E9 14 09 00 00` -> 0x828240 (caller fn 0x827760) | 0x7B9A27 `E9 F4 08 00 00` -> 0x7BA320 (caller fn 0x7B9860) | rel32 redirect of a TAIL JMP (E9), same pad mechanism | finder: evaluation = .pdata function holding `C7 81 D4030000 000080BF`; exactly one E8/E9 in .text targets its start (melee:502-510) | `eval_wrap` (melee:1014-1021) | each target evaluation of each vehicle-brain unit (each brain update) | W: TLS t_evalBrain (saved / restored, nesting-safe) | original's value | none |
| H2-H5 | the 4 `Weapon::SetTarget(weapon, eTarget*, a, mode, b)` calls inside the evaluation | 0x9A85CC, 0x9A86FC, 0x9A879F, 0x9A895A (all E8 -> 0x81F0C0) | 0x828855, 0x828939, 0x8289D2, 0x828B8D (E8 -> 0x6998E0) | 0x7BA94C, 0x7BAA7C, 0x7BAB1F, 0x7BACDA (E8 -> 0x631940) | call-site rel32 redirect, one pad per site | finder: SetTarget recognised by `48 83 B9 disp32 00` (first 0x30) and the type-0 clear pattern ending in `call Clear` (first 0x90) (melee:513-528); up to 8 sites kept | `st_wrap` (melee:1023-1043) | per weapon target hand-out in each evaluation | R: desc+8 (target type), weapon item tags, weapon+0x90 owner, entity flags, brain mode/records, weapon ranges, priority. W: none directly; calls the engine's clear target | original SetTarget's value, or `ClearTarget(weapon, 1)`'s value when holding (the SetTarget call is then skipped) | none |
| C1 | IAT entries for kernel32 ExitProcess / TerminateProcess in the exe, BugSplat64.dll, bdcap64.dll | - | today 0 hooked (Fauna was first) | - | IAT entry write (only entries still equal to kernel32's address) | value compare | hook_exit_process / hook_terminate_process (crash:42-57) | process exit | logs caller + 16 stack lines | calls the original | none |
| C2 | process-wide vectored exception handler (first) | - | - | - | AddVectoredExceptionHandler(1) | - | fx_veh (crash:91-113) | every exception (first-chance) | logs <= 8 fatal + <= 6 C++ throws | always CONTINUE_SEARCH | none |

Engine functions CALLED (never patched): Tag::Tag(const char*) (game 0x564E40, ed 0x3E44D0, srv 0x37D4B0; once per
process from ids_ready, the same ctor GEM Fauna / ScriptExt / Teleport log as `Tag(cstr)`; MeleeStance's `TagCtor` is a
different function, ed 0x3E4390), weapon range getter (game 0x81F660, ed 0x699EA0, srv 0x631EE0; per weapon per
vs_tick / st_wrap), ClearTarget = the engine weapon cease-fire (game 0x81F480, ed 0x699CC0, srv 0x631D00; = PenFilter's
"weapon stop", called with the same `(weapon, 1)`), eMoveTask FinishSub (game 0x91A340, ed 0x797E40, srv 0x72C610),
chassis vtable +0x168 (slot 45, SetMovementMode(chassis, mode, 0)), plus every original above. Located but never called:
GetCurrentWeapon (game 0x81B080, ed 0x695600, srv 0x62D900; only its layout gives the weapon list offsets), brain const
getter (ed 0x799C10), eOutdoorsBattle slots 3/4.
MatchAmmo (MeleeStance prologue hook, PenFilter A1/A2) is reached for vehicle units from function ed 0x8294D0 (stance
raw log: hook call from exe+0x82985C; `probe\chase\func_of.py`), which the evaluation calls per weapon slot (README:212,
inferred call path): so stance's and pen's ammo verdicts for a vehicle-brain unit run while chase's t_evalBrain is set,
before the SetTarget calls st_wrap sees.

## 3. Finders (how each address / offset is found, and whether another module's patch can disturb it)
Overlap check: `probe\chase\chase_probe.c` lists every byte window and whole-.text hit chase's finders read and
intersects them with the known code patches of the other two modules on each exe (MS MatchAmmo prologue; PF ctx
builder prologue, A volume / no-volume sites in MatchAmmo, B solver call, P try-loop head, X exhausted clear call;
addresses from `penfilter_0.1.1\out\selftest.txt` and `meleestance_0.7.42\build_log.txt`): result `none` on all three
exes. MeleeStance's other writes (eQuantExecutor vtable slot ed 0xF381D0, blow-reach table ed 0xF71368, ExecuteInstant
in 9 vtables, ...) are in .rdata / .data / unrelated vtables, none of which chase reads (chase reads only the eVS_Root,
eMoveTask, eSingleOrder, eMovementChange and eOutdoorsBattle vtables + RTTI). Reverse direction: PenFilter's
"weapon stop" (= chase's ClearTarget) is found through the weapon fire function's mode-0 stop call
(gem_penfilter.c:608-625), not through chase's SetTarget sites; MeleeStance identifies order classes from the order's
vtable pointer -> COL -> RTTI name (gem_meleestance.c:1435), which chase's slot swaps do not change; Teleport reads the
eSingleOrder vtable (tp/engine.c:231-238), which chase does not patch. Today's live order chase -> stance -> pen found
everything on the editor.

| finder (code) | what | method / anchors | windows (editor) | can a known MS / PF patch hit it? | can chase's own patches hit it? |
|---|---|---|---|---|---|
| find_weapon_layout (melee:267-282) | weapon+0x98 item, item tags +0x11E0/+0x11E8 | whole .text: `48 8B 80 i32 48 8B B8 e32 48 8B 98 b32`, all hits must agree | hits 0x782A80, 0x7D6078, 0x7DA518 (3 on every exe) | no overlap. A patch over a hit removes a hit (still OK while >= 1 remains) | no |
| find_weapons (melee:299-361) | Tag::Tag(cstr), const getter, fight distance field, eOutdoorsBattle slots, entity+0xDD8 weaponry, GetCurrentWeapon, list +0x30/+0x38 | string "fight/distance" in .rdata; exactly 2 `lea rdx,[str]` (48 8D 15); next two E8s after each; RTTI `.?AUeOutdoorsBattle@Human@@`; whole .text movss readers of +0x498 mapped to slots by .pdata; `mov rcx,[rcx+W]; call GetCurrentWeapon` in a slot; list read in GetCurrentWeapon | leas 0x782BD8, 0x7E1788; slots 0x7E1A10/0x7E1290 (+0x80); GetCurrentWeapon 0x695600 (+0x40) | no overlap. Count-sensitive: a hook on one of the two lea readers would make n != 2 | no |
| find_vehicle (melee:363-438) | eVS_Root / eMoveTask / eSingleOrder vtables; state->brain +0x30, mode +0x1E0, brain->entity +0x38, records +0x3A0; approach site + builder; AI order +0xF0; record pos +0x28/+0x2C; eMoveTask dtor / Update / FinishSub; sim clock | RTTI x3; tick = eVS_Root[3] (must be in .text, else silent fail); tick head 0xC0; whole-.text `movabs rax,"approach"` filtered to the tick by .pdata; E8 to a builder that leas the eMoveTask vtable; `cmp rax,[rcx+AI]`; movss pair from r12; dtor `test bl,1`; Update phase switch + `33 D2 48 8B 09 E8 .. 48 8B CB E8` (FinishSub, optional); clock = `mov eax,[rip+Q]; sub [rbx+..],eax` in eSingleOrder::Tick, minus 4, in .data, confirmed by a `mov eax,[rip]; ret` getter | tick 0x82F700..0x82FF00; builder 0x788A50 (+0x40); Update 0x78BC80 (+0xA0); dtor 0xAB720 (+0x20); eSingleOrder::Tick 0x797A60 (+0x40); clock getter 0x4DBF20 | no overlap. NOTE: if any dll swapped eVS_Root[3] or redirected the approach call BEFORE chase's finder runs, chase fails (slot not in .text / builder target not in .text). GEM Fauna (loaded earlier) names call sites ed 0x82F1C2 and 0x82F430 in its log (inferred hooked; Fauna source not read), both outside the tick window | chase's own V1 / V2 would break a SECOND run of the finder (no re-entry; init runs once) |
| find_priority (melee:444-476) | brain+0x3D4 approach range, record entity +0x18, entity+0x548 def, def props +0x330/+0x338, weapon range getter | tick 0x800: `movss xmm0,[rcx+A]; comiss ..` then `mov rdx,r12; call T`; in T `mov rsi,[rdx+E]` and the def props read; whole .text first `mov dword [rcx+A],-1.0`; then `mov rcx,rdi; call RANGE` + `movss [rsi+A],xmm6` | reset hit 0x828275 (1 hit on every exe) | no overlap | the reset pattern is inside the evaluation; chase patches only call rel32s there (SetTarget sites), not the reset |
| find_run_mode (melee:480-493) | entity+0xDE0 chassis, chassis vtable +0x168 SetMovementMode | RTTI `.?AUeMovementChange@Order@@` slot 19: `mov edi,imm32` (0x800..0x2000), `mov edx,[rbx+0x3C]` then `call [rax+disp]` | 0x78CF00 (+0x80) | no overlap | no |
| find_hold (melee:499-542) | evaluation start, its single caller, SetTarget + its sites, ClearTarget, weapon+0x90 owner | reset pattern -> .pdata start; count E8/E9 to it == 1; E8s inside the evaluation (by .pdata) to a function with the SetTarget shape; owner = `mov r,[rcx+OWN]; mov r,[r+0x568]` in SetTarget (0x568 HARD-CODED = weaponry), fallback whole-.text with o_weaponry | evaluation 0x828240 (+0x1400 scanned, function ends 0x828C20); SetTarget 0x6998E0 (+0x90) | no overlap. Count-sensitive: a redirect of the tail jmp by anyone makes the count 0 (hold OFF) | a second run after install would fail (count 0) |
| find_entity_tags (melee:554-604) | entity+0x4E8 tag vector | string "no_auto_repair"; static Tag init `lea rdx,str; lea rcx,TAG; call Tag(cstr)` (ctor must equal find_weapons'); every `lea rdx,[TAG]` with `mov rcx,[rax+0x38]; add rcx,C` near it, HasTag shape (two loads 8 apart, `add r,0x60`, call to the exact Tag== bytes `48 8B 02 48 39 01 0F 94 C0 C3`); all sites agree | lea site 0x82F3D4 (in function 0x82E010, the vehicle-AI function GEM Fauna also patches: Fauna's calls at 0x82F1C2 and 0x82F430 are outside chase's ~0x82F3C4..0x82F3F0 window, 0x40 bytes away), HasTag 0x10A3B0 | no overlap | no |

All finders need `.pdata` for func_of (find_weapons slot mapping, find_vehicle movabs filter, find_hold); without it
they fail and log. RTTI lookup = type descriptor in .data, COL in .rdata, first .rdata pointer to the COL; it compares
ABSOLUTE pointers, so the offline test must map the image at its preferred base (section 8).
What chase rewrites, precisely: the eVS_Root tick's code is NOT byte-patched (V1 is a vtable slot swap); inside the tick
function only the 4 rel32 bytes of the approach call change; inside the evaluation's caller the 4 rel32 bytes of the tail
jmp; inside the evaluation the 4 x 4 rel32 bytes of the SetTarget calls; in .rdata the three vtable slots.
Install-order conclusion: chase's finders read nothing MS or PF patch, and MS / PF finders read nothing chase patches
(known sites). The merged dll may run the three finder sets in any order; to stay byte-identical to today, run chase's
finders before any module installs and install chase first (plan order chase, stance, pen). The only hard rule: no
other module may patch eVS_Root[3], eMoveTask[0]/[19], the approach call, the evaluation's tail jmp or the SetTarget
call sites before chase's finders run (a future pen hook C on the approach must chain through chase's pad, not
re-find the site).

## 4. State
| state | size / key | lifetime / flush | notes |
|---|---|---|---|
| `g_ch[512]` Chase {order, ent, brain, cx, cy, r, reach, next, used, started, repath} (melee:610-624) | key = eMoveTask ORDER POINTER; lowest free slot; `g_chHigh`, `g_chLive` | slot freed by mt_update phase 0 (stale re-start) / phase 2 (stop), mt_dtor, or mission_check; full table = new approach silently not tracked | holds raw entity / brain pointers (no weak refs) |
| `g_tr[1024]` Track {ent, tgt, x, y, vx, vy, t} (melee:627-704) | key = chasing unit's ENTITY POINTER, hash `(e>>4)*2654435761 % 1024`, linear probe | only mission_check empties it; a slot is taken over only when its sample is older than 3000 ms of sim time | 0.5.3 address-independence argument (see R4c); target pointer compared, never dereferenced |
| interned ids `g_idMelee/2, g_idPropMelee/Ranged`, 4 static Tag buffers, `g_idState` (melee:130-157) | per process | built once lazily on the first engine call that needs them; never rebuilt or freed; not reset at a new battle | 0 none / 1 building / 2 ready / 3 off |
| trust counters `g_idChecks`, `g_idPos` (melee:160-175) | per process | never reset | ids used alone once checks > 400 and hits > 5; any disagreement before that = ids off for good (name compares) |
| `g_epochLast`, `g_epochLogged` (melee:644-665) | per process | updated on every vs_tick and on mt_update while chases live | new mission = clock stepped back or jumped > 5000 ms |
| `g_tagPrioSeen[20]`, `g_tagPrioLogged`, `g_tagPrioScans` (melee:764-789) | entity pointer + prio | process; log only | stale pointers survive missions (log effect only) |
| log caps (process lifetime, never reset): g_regLogged 20, g_repLogged 30, g_stopLogged 20, g_pursueLogged 20, g_prioLogged 20, g_holdLogged 10, g_noMeleeLogged 15, g_epochLogged 20, cxx 6, g_vehCount 8 | | | after the cap a line type is silent for the rest of the process (all battles) |
| TLS (`_Thread_local`): t_apOrder, t_apX, t_apY, t_apR, t_chase, t_haveV, t_mel, t_vx, t_vy, t_evalBrain (melee:617-621, 1009) | per thread | per call | vs_tick -> ap_move hand-off; eval_wrap -> st_wrap hand-off |
| `static HANDLE hs[1024]` in patch_code (patch:69) | | init only | not re-entrant |
| ini values (section 6) | per machine | process | must be equal on every MP peer (they steer decisions) |

New battle: mission_check (melee:648-665) clears g_ch, g_chHigh, g_chLive and g_tr under g_cs; nothing else. Called
at the top of every vs_tick (melee:855) and of mt_update when g_chLive > 0 (melee:930). Within a mission a live chase
calls it every quant, so no mid-mission clear; a > 5 s sim gap with no live chase and no vehicle tick does clear, but
then g_ch is empty and every g_tr sample is older than 3 s (= restarted with velocity 0 anyway), so the result equals no
clear. A merged core detector at quant start with the same rule (step back, or > 5000 ms jump) is therefore
equivalent, provided it flushes before the first chase call of the new mission.
Wall clock: `GetLocalTime` in logf_ (core:22, log prefix only) and `Sleep(5)` in suspend_others (patch:58, init
only). No GetTickCount / QueryPerformanceCounter / timeGetTime. No wall-clock value reaches a decision. All decision
timing (`next` re-path checks, track dt / staleness, epoch) uses `*g_simTime` (dword, ms; game 0x1584568, ed 0x13DF988,
srv 0x11CBA58 = the same address MeleeStance and PenFilter find).
Per-machine influences on decisions: ini values; id trust timing (same answers unless ids and names disagree);
which unit loses the velocity sample when > 1024 fresh samples exist AND brain ticks run on several threads (inferred
edge; whether vehicle AI runs on more than one thread is not established).

## 5. Helpers a shared core could replace (exact semantics)
| helper (code) | semantics |
|---|---|
| logf_ (core:18-28) | vsnprintf to 1024 chars; line `HH:MM:SS.mmm [GEM MeleeChase] <text>\n` with LOCAL wall time; fputs + fflush to g_log if open; stdout if console; always OutputDebugStringA; no own lock (CRT FILE lock per call); FX_TEST = printf + newline |
| ini_str / ini_int / ini_float (core:30-44, main:7-14) | path = dll folder + `\gem_meleechase.ini`, section `meleechase`; str: key <= 63 chars, default <= 127, value read as 256 wchar, narrowed CP_ACP, cut to buffer size; int: GetPrivateProfileIntW (decimal, leading '-' ok, "1.5" = 1); float: `%g` default -> strtof, unparsable / NaN = default, clamped |
| get_sections (core:56-71) | exe image (GetModuleHandleW(NULL)); `.text .rdata .data .pdata` by name, VirtualAddress + VirtualSize; timestamp |
| in_text / in_rdata / in_data (core:74-76) | half-open range test against the exe section |
| func_of (core:98-103) | .pdata binary search, follows indirect (u&1) and chained (UNW_FLAG_CHAININFO) entries up to 8 levels; returns the primary function's RVA, 0 if unknown |
| find_bytes, find_cstr, next_call, find_lea_to, find_rip_ref (unused), prologue_len, rel32_target, rd_u32, EXE_OFF (core:105-180, melee:124-125) | raw byte scans, no instruction decoding (an E8 byte inside another instruction counts if its target lands in .text) |
| find_vtable_rtti (core:183-200) | decorated name in .data (type descriptor = name - 16), COL in .rdata with sig 1, offset 0, self RVA check; vtable = first .rdata qword pointing at the COL, + 8. Absolute pointer compare (needs the image at its preferred base offline). MeleeStance has a different-signature function of the same name |
| patch writer: alloc_near, suspend_others, patch_code, patch_call_rel32, redirect_call, vt_hook, install_jmp5 (unused) (patch:10-137) | alloc_near: first free granule above, then below, within +-0x7FF00000, RWX, one 64 KB reservation per call. patch_code: VirtualProtect RWX, suspend every other thread (retry 20 x while any RIP is inside the bytes, Sleep 5), memcpy, resume, restore, FlushInstructionCache; refuses when threads cannot be stopped. patch_call_rel32 writes only the 4 rel32 bytes (no opcode / old-value check). redirect_call: pad `FF 25 00000000 abs64`, orig = old target. vt_hook: VirtualProtect RW, orig = old slot, store, no suspension, no old-value check |
| memory probes | none on any hot path: every engine read is a raw dereference with null checks (and size caps on vectors: item tags <= 256, def props <= 256, entity tags <= 1024, weapons <= 64). ReadProcessMemory only in log_stack (crash), VirtualQuery only in alloc_near (init) |
| tag readers (melee:122-258) | MsString (MSVC std::string: SSO when cap <= 15, size at +0x10), Tag = 0x60 bytes, name at +0x40, interned id = first qword. tags_have_melee: melee_tag or vehicle_melee_tag, id compare and/or case-insensitive exact-length name compare; tags_priority_bits: the two priority names, bit 1 melee / bit 2 ranged; id trust as in section 4; ids are built from the LOWER-CASED names with the engine Tag(cstr) |
| weapon_is_melee (melee:200-206) | weapon+0x98 -> item -> item tags +0x11E0..+0x11E8 contain a melee word; null checks only |
| unit_reach (melee:724-741) | entity+0xDD8 weaponry -> list +0x30..+0x38 (<= 64); per non-null weapon: melee? and engine range (world units, > 100000 -> 0); returns the largest melee range (0 = none, -1 = melee with unknown range), *ranged = largest non-melee range |
| unit_priority (melee:790-802) | entity tags (if found) first, else def props; 2 melee / 1 ranged / 0 none; melee wins inside a level |
| human / vehicle / alive (melee:28, 858, 1028) | `entity[0x30] & 4` = human (skipped), `& 2` = alive / in scene; bit 3 (vehicle class) unused; HARD-CODED offset |
| entity position (melee:29) | float x, y, z at entity+0x74 (HARD-CODED; Teleport finds the same +0x74); distances: 2D (dist2d) except apply_priority (3D, record z read at record+0x30, inferred) |
| direct control, owner / AI-or-player, linked, entity id, weak refs | NONE in this module: chase never checks direct control or the owning player, never reads entity ids, keeps raw pointers (guarded by the dtor hook + mission_check) |
| sim clock | `*g_simTime` (volatile dword) read directly |
| engine cease-fire | `g_clearTarget(weapon, 1)` (= PF's e_wstop(weapon, 1)) |
| crash log (crash:13-113) | see section 6c |

## 6. Outside interface
Exports: none of its own (`DllMain` is not exported); the live dll's export table holds 47 mingw CRT helper names
(auto-export, e.g. `_CRT_INIT`, `__mingw_TLScallback`, `atexit`; `probe\chase\pe_info.py`). Nobody imports from it
(Teleport looks up only `gem_meleestance.dll!gem_quant_listen`). Imports: KERNEL32 (AddVectoredExceptionHandler,
AllocConsole, CloseHandle, CreateToolhelp32Snapshot, DeleteCriticalSection, DisableThreadLibraryCalls,
EnterCriticalSection, FlushInstructionCache, GetCurrentProcess(Id), GetCurrentThreadId, GetLastError, GetLocalTime,
GetModuleFileNameW, GetModuleHandleExW, GetModuleHandleW, GetPrivateProfileIntW/StringW, GetProcAddress, GetProcessId,
GetSystemInfo, GetThreadContext, InitializeCriticalSection, LeaveCriticalSection, MultiByteToWideChar, OpenThread,
OutputDebugStringA, ReadProcessMemory, ResumeThread, SetConsoleTitleA, Sleep, SuspendThread, Thread32First/Next,
TlsGetValue, VirtualAlloc/Free/Protect/Query, WideCharToMultiByte) + UCRT api-ms-win-crt-* (strtof, wcsrchr, stdio,
string, heap, runtime). `-luser32` is linked but nothing is imported from it. Has a TLS directory (`_Thread_local`).

### 6a. ini (`[meleechase]` in gem_meleechase.ini; snapshot ini == dist ini == live payload a1d6ed3c ini, diff empty)
| key | type | code default | clamp / rule | snapshot ini | live ini |
|---|---|---|---|---|---|
| enabled | int | 1 | 0 = silent, nothing patched | 1 | same |
| log | int | 1 | log file | 1 | same |
| console | int | 0 | AllocConsole | 0 | same |
| verbose | int | 1 | read, never used | 1 | same |
| crash_log | int | 1 | VEH + exit trace | 1 | same |
| log_chase | int | 1 | play lines (all capped) | 1 | same |
| melee_tag | str (47) | melee | empty = dll does nothing | melee | same |
| vehicle_melee_tag | str (47) | veh_melee | empty = off | veh_melee | same |
| vehicle_chase | int | 1 | 0 = vehicle part off | 1 | same |
| repath_fraction | float | 0.25 | [0, 1] | 0.25 | same |
| repath_min_m | float | 1 | [0.2, 100], x20 | 1 | same |
| repath_max_m | float | 10 | [0.2, 500], x20, >= min | 10 | same |
| repath_interval_ms | int | 500 | [100, 10000] | 500 | same |
| repath_interval_close_ms | int | 200 | [50, 10000] | 200 | same |
| close_range_m | float | 10 | [0, 200], x20 | 10 | same |
| contact_factor | float | 0.5 | [0.1, 1] | 0.5 | same |
| lead_seconds | float | 0.6 | [0, 5] | 0.6 | same |
| lead_max_m | float | 6 | [0, 50], x20 | 6 | same |
| pursue_retreating | int | 1 | | 1 | same |
| retreat_speed_mps | float | 0.3 | [0, 20], x20 | 0.3 | same |
| chase_always | int | 1 | | 1 | same |
| chase_run_mode | int | 2 | > 7 -> 2; < 0 = don't set | 2 | same |
| hold_ranged_in_melee | int | 1 | | 1 | same |
| hold_ranged_reach_factor | float | 1.0 | [0.2, 5] | 1.0 | same |
| smooth_repath | int | 1 | | 1 | same |
| weapon_priority | int | 1 | | 1 | same |
| prop_prioritize_melee | str (47) | ai_prioritize_melee | also the entity tag name | ai_prioritize_melee | same |
| prop_prioritize_ranged | str (47) | ai_prioritize_ranged | also the entity tag name | ai_prioritize_ranged | same |
| human_fight_distance, fight_distance_mult | - | - | ignored since 0.5.0 (main:42) | absent | absent |

### 6b. Float computations that feed simulation decisions (build.bat lacks the merged flags)
ini_float parse + clamp + `x 20.0f` (main:43-61); weapon range compare `> 100000` and max (melee:735-738); approach
range 3D distance `sqrtf(dx*dx+dy*dy+dz*dz)` vs reach, write `reach` / `-1.0f`, compare `*cur != want` (melee:817-821);
dist2d (melee:706) in vs_tick (always / retreat tests, melee:879-881), registration (melee:900), mt_update (melee:968,
974), st_wrap hold test `<= mel * hold_factor` (melee:1034); receding = projection `(dx*vx+dy*vy)/d` (melee:717-721) vs
retreat speed (melee:881, 948, 976); lead point `v * lead_s`, clamp by `lead_max / l` (melee:709-714) -> circle centre
(melee:750) and `moved` (melee:974); contact radius `max(mel * contact, 10)` (melee:747-749); re-plan threshold
`clamp(self * frac, min, max)` (melee:982-984); `self <= reach * 0.9f` (melee:976), `d > mel * 0.9f`, `d > mel *
contact` (melee:880-881); velocity EMA `(x - t.x) * 1000 / (float)dt`, `0.5*v + 0.5*inst` (melee:697-698).
Result of the rebuild test (R7): `-mcpu=baseline` and `-fno-fast-math` change nothing (byte-identical dll: zig already
defaults to baseline for an explicit `-target`, and fast-math is off by default); `-ffp-contract=off` changes the code
of melee.c:706, 719, 818-819, 948, 968 only, by packing the x / y lanes into mulps / subps instead of scalar mulss /
subss (same count of sqrtss, divss, ucomiss). Per-lane SSE results equal the scalar ones and the only reorder is the
commutative `a+b` -> `b+a`; LLVM does not reassociate without fast-math flags and baseline x86-64 has no FMA, so every
result is bit-identical. Conclusion: compiling chase with the merged flags cannot change a decision (inferred from the
asm diff + IEEE rules; no runtime A/B was run).

### 6c. Log, words, events
- Log file: `<dll folder>\gem_meleechase.log`, truncated at every start, one name for every exe (the dedicated server's
  copy lives in `<payload>\server\`, so no clash). Line format `HH:MM:SS.mmm [GEM MeleeChase] <text>`.
- Lines other notes rely on: conquest.md:22 counts `melee unit ... approaches ... following the target`,
  `melee-priority`, `ranged-priority ... hold (in reach)` and `approaches but has no weapon tagged "melee"` (0.5.2 server
  log); since 0.5.4 the `ranged-priority` / `ranged reach` variants of the apply_priority line are dead code (ranged
  returns before the log, melee:812). README:137-157 lists the expected start-up and play lines; README:145 / 157
  ("ranged fallback") and `_notes\dreadnought.md:219` describe the interim `ranged_fallback` heuristic that the final
  0.5.4 REMOVED (dll_hooks.md:174 "The interim ranged_fallback heuristic is removed"); no such code or log line exists.
  Start-up lines are column-aligned (`vehicle melee chase            ON (...)`).
- Game-side words read: weapon item `{tag}` words `melee` and `veh_melee` (ini); def `{props}` and entity tags
  `ai_prioritize_melee` / `ai_prioritize_ranged` (ini); engine strings used as finder anchors: `fight/distance`,
  `approach` (8-byte immediate), `no_auto_repair`, RTTI names `.?AUeOutdoorsBattle@Human@@`, `.?AUeVS_Root@Actor@@`,
  `.?AUeMoveTask@Order@@`, `.?AVeSingleOrder@@`, `.?AUeMovementChange@Order@@`. Mod users: `dread_limbs`
  (mech_dreadnought.inc) adds / removes entity tag `ai_prioritize_melee` (dreadnought.md:238); dread defs carry the props.
- Set / fired: no tags, props or script events are written; engine state written = brain approach range, approach
  circle, chassis movement mode (via the engine setter), weapon targets cleared (via the engine call), move orders ended
  early (Update returns 1) or finished without the chassis stop (FinishSub).
- Crash log: fx_veh is process-wide and first; it logs any module's fault (addr_desc names the module), and log_stack
  shows return addresses inside the exe .text or inside g_self. In one combined dll, g_self covers all three modules, so
  ONE copy serves the whole dll; the exit IAT hooks are process-wide too (and are a no-op while GEM Fauna, loaded
  earlier, already holds those IAT entries). Caution (inferred): fx_veh calls logf_ (CRT stdio locks) inside the
  exception context; a fault while the same thread holds the log FILE lock would deadlock.

## 7. Link-level facts
- Non-static symbols: `DllMain` (main:122) and, under FX_TEST, `main` (test:7). Everything else is `static` (functions,
  globals, tables) -> no link collision when chase is its own translation unit. Within ONE translation unit with the other
  modules there would be many: logf_, find_bytes, find_cstr, next_call, find_vtable_rtti and func_of / in_text (different
  signatures in MeleeStance), g_base, g_text, g_cs, g_log, g_dir, g_self, ini_* ... So: separate TU, as planned.
- No `__asm__` blocks, no global asm labels, no naked functions, no .def file, no dllexport.
- Macros with generic names: FX_TEST, FX_VERSION, FX_NAME, FX_INI, FX_LOG, FX_SECT, FX_MAX_THREADS, EXE_OFF, ENT_FLAGS,
  ENT_POS, WORLD_SCALE, TAG_SIZE, TAG_NAME, MAX_HSLOTS, MAX_ST_SITES, MAX_CHASE, MAX_TRACK, TRACK_STALE_MS,
  EPOCH_GAP_MS, IDS_TRUSTED, PRIO_MELEE, PRIO_RANGED, TAG_PRIO_LOG_MAX, TAG_PRIO_SCAN_MAX. Typedefs: Sec, FxRtf,
  MsString, Chase, Track, GetWeaponFn, TagCtorCStrFn, ConstGetFn, SlotFn, MoveCircleFn, UpdateFn, FinishSubFn, DtorFn,
  WeaponRangeFn, SetTargetFn, ClearTargetFn, EvalFn, ExitProcessFn, TerminateProcessFn, local SetModeFn. Only a
  problem if a shared header is included before them.
- Thread-locals: 10 `_Thread_local` variables -> static TLS (.tls) in the dll; works for a LoadLibrary'd dll on
  Vista+ (the live dll already has a TLS directory).
- Headers: windows.h, tlhelp32.h, wchar.h, stdlib.h, stdio.h, stdint.h, string.h, stdarg.h (FX_TEST: sys/mman.h,
  strings.h, `#define _strnicmp strncasecmp`, `typedef int LONG`).
- Compiler assumptions: GNU C (zig cc / clang): `__attribute__((unused))`, `__attribute__((aligned(16)))`,
  `__builtin_sqrtf`, `__builtin_return_address`, `__builtin_frame_address`, `_Thread_local`; unaligned
  `*(uint32_t *)(p+n)` reads; mingw CRT names `_wfopen`, `_strnicmp`, `_wcsnicmp`. Build: `-O2 -s -shared -Wall -Wextra
  -Wno-unused-parameter -Wno-missing-field-initializers`, `-lkernel32 -luser32`, no -mcpu / -ffp-contract /
  -fno-fast-math. Clean under -Wall -Wextra (probe build).
- Global CRT state touched: `freopen("CONOUT$", "w", stdout)` when console=1 (process-wide stdout).

## 8. Self-test
- Today: Linux / WSL only (`tests/run_offline_check.sh`): gcc -O1 -DFX_TEST builds `out/mc_test` from the unity file;
  test_main.c mmaps the exe at its ImageBase with MAP_FIXED_NOREPLACE, copies raw section bytes, sets the 4 section
  globals and runs the 7 finders. Nothing is faked, no hooks, no behaviour cases (no chase / priority / hold / track /
  epoch test exists in the snapshot; README:8's "tested offline against fake entities" test is not in it).
  `ids_ready()` returns 0 under FX_TEST. Then a clang -fsyntax-only -Werror check of the Windows build against the fake
  headers in tests\stub. `tests/check_entity_tags.py` (pefile) cross-checks +0x4E8 by a second route.
- Windows port (done for this map, `probe\chase\chase_probe.c`, works): include core.c + melee.c unchanged with
  `#define FX_TEST 1` and `<windows.h>` instead of S (S's FX_TEST branch pulls sys/mman.h and `typedef int LONG`);
  map with `VirtualAlloc((void*)ImageBase, SizeOfImage, MEM_RESERVE|MEM_COMMIT, PAGE_READWRITE)` (0x140000000 for all
  three exes; free because the zig test exe is ASLR-relocated), copy headers + each section (min(raw, virtual)), set
  g_base / g_text / g_rdata / g_data / g_pdata. If the base is taken, map anywhere and apply the .reloc DIR64 entries
  (implemented, not needed). The image MUST be at its preferred base or relocated: find_vtable_rtti compares absolute
  COL pointers and the vtable slot values are used as code addresses. The three runs take a few seconds in all.
- To make it a function a combined test exe can call: rename `main` (test:7) to e.g. `int chase_selftest(void)` that
  assumes the core already mapped the image and filled chase's section globals (or reads them from the core), keep the
  7 finder calls and the printouts, return ok; replace `printf` with the core test logger; drop the file reading /
  mmap (core does it once per exe). Because chase's finders keep their results in statics (o_*, g_*), running the same
  module on a second exe in one process needs a reset function (zero every o_* / g_* found by the finders, g_idState,
  g_okClose / g_okPriority / g_okHold...) or one process per exe. Add behaviour cases with fake entities (layout =
  the found offsets): priority precedence (tag melee > tag ranged > prop melee > prop ranged), apply_priority write /
  no write, hold verdict, track determinism under different addresses, epoch reset, mt_update phase handling.

## 9. Port list (smallest edits; snapshot line numbers)
1. `S:26-47`: drop the FX_TEST include branch (or rename the macro to CHASE_TEST); `FX_NAME` used by logf_ (core:24),
   the console title (main:27) and the banner (main:29) -> module tag `chase`; `FX_INI` / `FX_LOG` / `FX_SECT` -> core
   ini path + section name kept as `meleechase` (plan: every old section / key keeps its name).
2. `S:51` (crashlog.c): do not include; the core owns the VEH + exit trace (one copy serves all, section 6c).
3. `S:53` (test_main.c): replace with `chase_selftest()` (section 8).
4. `core:10-44`: logf_ -> a static wrapper that forwards to the core sink (`[chase]` prefix, sim ms); ini_str / ini_int
   -> core ini reader with section `meleechase` (same keys, same defaults, same parse rules: GetPrivateProfile semantics).
   Delete g_log / g_console / g_logfile / g_dir.
5. `core:56-71` get_sections -> fill g_base / g_text / g_rdata / g_data / g_pdata from the core's image info (keep the
   statics so the finders stay unchanged).
6. `patch.c` (whole file): keep the function names, route `patch_code` (patch:65-77) and `vt_hook` (patch:130-137)
   through the core patch registry (claim + expected bytes): pass the expected old rel32 / opcode for patch_call_rel32
   (patch:108-114) and the expected old slot value for vt_hook. alloc_near can come from a core pad pool (one 64 KB
   reservation per pad today; 6 pads).
7. `main:16-33` init -> `int chase_attach(const core_api *)`: delete g_self / g_dir / exe gate / log / console /
   crash_log (main:18-23, 25-27, 30-33; the core does them); keep `enabled` (main:24) as the module switch; banner ->
   one core line. `main:35-120` unchanged. `main:122-131` DllMain -> delete.
8. `melee.c:422-431` sim clock: keep the finder (parity check) but compare with / take the core's clock pointer; log
   a mismatch.
9. `melee.c:644-665` mission_check: either keep it unchanged (self-contained, sim clock only) or make its body ask the
   core's new-battle counter and call a new `chase_flush()` (= lines 655-657) on change; keep the two call sites
   (melee:855, 930) so the flush still precedes the first use (section 4 equivalence).
10. Exports for the other modules (new non-static wrappers, no behaviour change): `int chase_unit_priority(void *e)`
    (unit_priority, melee:790), `float chase_unit_reach(void *e, float *ranged)` (melee:724), `int
    chase_weapon_is_melee(void *w)` (melee:200), `int chase_hold_verdict(void *brain, void *w)` (the test in st_wrap
    melee:1026-1040 split out, so pen's R3 can ask before stopping a weapon and chase can ask pen), `int
    chase_is_chasing(void *e)` (scan g_ch by ent, under g_cs), `void chase_flush(void)`, read-only access to the found
    layout (o_weaponry, o_wlistBeg, o_weapItem, o_itemTagBeg, o_entTags, o_entDef, o_defPropBeg, o_weapOwner,
    g_clearTarget, g_weaponRange). Later (flanking plan, not 0.1.0): `chase_flank_state`.
11. Optional, behaviour-neutral: drop `verbose` (unused); remove the dead `ranged-priority` log branch only if parity
    of log text is not required.

## 10. Risks and questions
Risks
1. No player / direct-control / owner check anywhere: chase acts on any vehicle-brain unit whose root state runs (no
   user order), player-owned ones included; st_wrap clears targets of player-owned melee vehicles' guns in the AI
   evaluation too (player attack orders take another path, README:112). R6 of the plan must decide whether to keep this.
2. Hold rule (st_wrap) measures the distance to the brain's FIRST target record, not to the target SetTarget is
   handing out, and clears every non-melee weapon of the unit: a gun the AI (or pen) wants on a second target is
   cleared too while the first target is in melee reach. Conflicts with the plan's R3 wording.
3. Hard-coded layout: ENT_FLAGS 0x30, ENT_POS 0x74, TAG_SIZE 0x60, TAG_NAME 0x40, WORLD_SCALE 20, and 0x568 inside
   find_hold's owner pattern (melee:531; fallback uses the found o_weaponry). A game update can move them without any
   finder noticing (except 0x568).
4. Tail-jmp redirect: eval_wrap is entered by the evaluation caller's tail jmp and forwards 4 integer registers only;
   correct only while the evaluation takes <= 4 integer arguments and no float register arguments (inferred; works live).
5. TLS hand-off vs_tick -> ap_move assumes no nested vs_tick on the same thread between setting t_* and the approach
   call (inferred safe).
6. Today's editor log shows a melee-priority unit re-issuing its approach every ~20-25 ms at a constant 5.8 m
   (12:27:42-12:27:44) and holding / "chasing" every ~6 ms: an order churn the plan's R4 flip detector should also
   watch on vehicle units; cause not established from the source.
7. Interned ids: built once per process from lower-cased names, never refreshed; after 400 agreeing checks they are
   trusted without the name check; a later disagreement (e.g. engine tag table reset, upper-case tag) would go unseen.
8. Tables: 512 chases / 1024 velocity samples, silent drop when full.
9. Partial installs are not rolled back (install_vehicle / install_hold); with a patch registry decide whether a
   failed module rolls back its own writes.
10. vt_hook stores without suspending threads (aligned 8-byte store, atomic) and without checking the old value;
    patch_call_rel32 does not check the old rel32 (both fine at injection time, unsafe if ever called late).
11. Exit trace is dead code in today's payload (Fauna first); a merged dll that keeps it changes nothing.
12. README / notes drift: README:145,157 and dreadnought.md:219 describe the removed ranged fallback; README "Engine
    facts" are 1.067 addresses; the src header still says "1.067" (S:2).
13. chase_run_mode calls chassis vtable slot 45 (SetMovementMode); GEM Fauna hooks movement-mode machinery for foot
    chassis (`sprint: eChassisFoot vt ... SetMovementMode slot 45`): for foot-chassis fauna the call may go through
    Fauna's code (inferred, Fauna source not read).
14. mt_update runs for every eMoveTask in the game (including MeleeStance's charge orders): while any chase is live,
    every move order update of every unit takes g_cs and scans up to 512 slots (cost; plan R8).
Questions
- Q1: Keep chase's behaviour for player-owned vehicle-brain units and for units under direct control (no check today)?
- Q2: Keep st_wrap's first-record distance and "clear every non-melee weapon" (parity), or align with pen's per-target
  verdict (R3)?
- Q3: Does the engine run vehicle brain updates on more than one thread? (decides whether shared per-sim-clock caches
  need locks and whether the > 1024 track edge can differ between peers)
- Q4: Is the 20-25 ms approach churn in today's log (12:27:42-44) expected or a bug to look at before the merge?
- Q5: Merged log name / prefixes: the start-up lines are column-aligned for a 30-char label; the plan's
  `[chase]` prefix changes every line others may grep (conquest.md:22 counts lines by text).
- Q6: Should `verbose` (unused) and the dead ranged-priority log text be dropped, or kept for byte-for-byte log parity?
<!-- numbered sections end -->

---------------------------------------------------------------------------------------------------------------------
# Raw reading notes (in source order)

## R1. Raw notes: S (gem_meleechase.c, 53 lines)
- Unity file. Only translation unit. `S:26-37` includes: non-test `<windows.h> <tlhelp32.h> <wchar.h> <stdlib.h>`;
  FX_TEST `<stdlib.h> <sys/mman.h>`, `#define _strnicmp strncasecmp`, `typedef int LONG;`, `<strings.h>`. Always
  `<stdio.h> <stdint.h> <string.h> <stdarg.h>` (`S:38-41`).
- Macros `S:43-47`: `FX_VERSION "0.5.4"`, `FX_NAME "GEM MeleeChase"`, `FX_INI L"\\gem_meleechase.ini"`,
  `FX_LOG L"\\gem_meleechase.log"`, `FX_SECT L"meleechase"` (the ini section name, wide).
- Include order `S:48-53`: core.c, patch.c, melee.c, crashlog.c, main.c, test_main.c.
- Header comment `S:2` still says "GEM engine, 1.067" (stale; live log runs on 1.069 exes, see R7).

## R2. Raw notes: core (core.c, 200 lines; "copied from GEM Fauna 0.5.1", same helpers as ScriptExt core.c, core:1-4)
- `g_verbose = 1` (core:10) static int; read from ini key `verbose` (main:25), used nowhere else (grep).
- FX_TEST `logf_` = vprintf + "\n" (core:12). Non-test (core:14-44):
  - `static FILE *g_log; static int g_console, g_logfile = 1; static wchar_t g_dir[MAX_PATH];`
  - `logf_(fmt,...)` (core:18-28): vsnprintf into `char buf[1024]`, `GetLocalTime` -> line
    `"%02d:%02d:%02d.%03d [GEM MeleeChase] %s\n"` (wall-clock local time, `char line[1100]`), fputs+fflush to g_log if
    open, to stdout if g_console, always `OutputDebugStringA(line)`. NO lock (relies on CRT FILE lock inside fputs;
    two threads can interleave whole lines but not chars, inferred from MSVCRT/UCRT FILE locking). Not sim time.
  - `ini_str(key, def, out, n)` (core:30-37): path = g_dir + FX_INI; key widened to 64 wchar, def to 128 wchar,
    `GetPrivateProfileStringW(FX_SECT, ...)` into 256 wchar, narrowed CP_ACP; on narrowing failure `out[n-1]=0`.
  - `ini_int(key, def)` (core:39-44): `GetPrivateProfileIntW(FX_SECT, key, def, ini)` cast to int (so "-1" works:
    GetPrivateProfileInt parses a leading '-' and returns the UINT of a negative, cast back = -1; standard Win32 behaviour).
- PE (core:50-72): `typedef struct { uint8_t *p; size_t n; } Sec;` statics `g_text, g_rdata, g_data, g_pdata`,
  `g_base`, `g_timestamp`. `get_sections()` (non-test, core:56-71): `g_base = GetModuleHandleW(NULL)` (the EXE),
  walks section headers, matches `.text/.rdata/.data/.pdata` by `strncmp(...,8)`, uses `VirtualAddress` and
  `Misc.VirtualSize`; returns ok if text, rdata, data found (pdata optional). Timestamp = FileHeader.TimeDateStamp.
- `in_text` / `in_rdata` (unused attr) / `in_data` (core:74-76): half-open range checks against the section.
- `.pdata` lookup (core:80-103): `FxRtf {b,e,u}`, `rtf_find(rva)` binary search, `rtf_primary` follows indirect
  (u&1) and CHAININFO (flag bit 4 of `ui[0]>>3`) up to 8 levels; `func_of(addr)` = rva of the primary function start,
  0 if unknown or no pdata.
- `find_bytes(p,n,pat,m)` naive first-byte + memcmp (core:105-111).
- `find_cstr(s)` (core:114-120): NUL-terminated string in .rdata starting at a string boundary (prev byte 0 or rdata
  start). Includes the terminating NUL in the match.
- `next_call(p, window, &site)` (core:123-131): first E8 in window whose rel32 target is in .text; returns target,
  site optional. No instruction decoding (any 0xE8 byte, e.g. inside an immediate, counts if target lands in .text).
- `find_lea_to(want, which)` (core:134-144): k-th `REX.W? 8D /r [rip+disp32]` (REX byte must be 0x48-0x4F, modrm
  &0xC7 == 0x05) in the whole .text whose target == want.
- `find_rip_ref(from,n,want)` (core:147-160, unused attr): optional REX, opcodes 8D 8B 89 39 3B 83, modrm rip;
  for 83 adds 1 to target.
- `prologue_len(f)` (core:163-178): accepts push r64, mov r64,r64 (89/8B mod=11), `mov [rsp+disp8],r` (89 44 24),
  `sub rsp,imm8` (83 EC), `sub rsp,imm32` (81 EC); stops at >=5 bytes; returns 0 for anything else (incl. RIP-rel,
  and any 0xE9/0xFF jmp written by another hook dll) and if >15.
- `EXE_OFF(p)` macro (core:180) = p - g_base as unsigned long long (all log addresses are exe offsets = RVAs).
- `find_vtable_rtti(decorated)` (core:183-200): type descriptor name searched in .data (`td = p - 16`), then a COL in
  .rdata with sig 1, offset 0, typedesc rva, self rva == col; then the first .rdata qword == &COL; vtable = that + 8.
  Only offset-0 COL (primary vtable). Linear scans of .rdata (O(rdata^2) worst case for the inner loop per COL hit).

## R3. Raw notes: patch (patch.c, 138 lines; non-test only, patch:8-138)
- `alloc_near(target, size)` (patch:10-30): walks up from target+gran with VirtualQuery, VirtualAlloc
  RESERVE|COMMIT PAGE_EXECUTE_READWRITE at the first MEM_FREE granule; then walks down by gran trying VirtualAlloc
  blindly. Range +-0x7FF00000. Every call allocates a fresh granule (64 KB reserved) even for a 0x40 pad.
- `suspend_others(hs,max,lo,hi)` (patch:36-61): FX_MAX_THREADS 1024; up to 20 attempts: Toolhelp snapshot, every
  other thread of this process OpenThread(SUSPEND_RESUME|GET_CONTEXT), SuspendThread, GetThreadContext CONTEXT_CONTROL;
  if any Rip in [lo,hi) -> resume all, Sleep(5), retry. Returns n or -1 (no snapshot / more than max threads / 20 tries).
  Threads that cannot be opened are skipped silently (not suspended).
- `resume_others` (patch:62).
- `patch_code(dst,src,len,what)` (patch:65-77): VirtualProtect RWX, `static HANDLE hs[1024]` (comment: init runs once,
  on one thread: NOT re-entrant / thread-safe), suspend, memcpy, resume, restore protect, FlushInstructionCache.
  Returns 0 and logs `"%s: VirtualProtect failed (%lu)"` or `"%s: could not stop the other threads safely - not patched"`.
- `install_jmp5(f,hook,orig,what)` (patch:82-105, `unused` attr): prologue_len -> stub = alloc_near(f,0x1000):
  stub+0 `FF 25 00000000 <abs64 hook>`, trampoline at stub+0x40 = original prolen bytes + `FF 25 0 <abs64 f+prolen>`;
  `*orig = tr` before the patch; patch = E9 rel32 + 0x90 padding to prolen. Logs "prologue not relocatable (<12 bytes
  hex>) - not hooking", "no memory within 2GB for the stub", "stub too far". (Never called: melee.c uses only redirect_call and vt_hook; grep.)
- `patch_call_rel32(e8,pad,what)` (patch:108-114): rewrites the 4-byte rel32 at e8+1 (not the E8 opcode) via
  patch_code. Does NOT verify that e8[0]==0xE8 or what the old rel32 was (callers must).
- `redirect_call(e8,fn,orig,what)` (patch:118-127): pad = alloc_near(e8,0x40) = `FF 25 0 <abs64 fn>`;
  `*orig = e8+5+rel32` (the old target, read before patch); then patch_call_rel32. On failure frees pad.
- `vt_hook(vt,slot,fn,orig)` (patch:130-137): VirtualProtect 8 bytes RW, `*orig = vt[slot]; vt[slot] = fn;` restore.
  NO thread suspension, no verification of the old pointer (caller), not atomic-flagged but an aligned 8-byte store
  is atomic on x64. Note: PAGE_READWRITE on a .rdata vtable page; if the page shares code (it does not, .rdata) fine.

## R4. Raw notes: melee.c (1062 lines), part 1 (lines 1-477)
### Header / layout constants (melee:1-118)
- `ENT_FLAGS 0x30` (byte: bit1 alive/in scene, bit2 human, bit3 vehicle class), `ENT_POS 0x74` (float x,y,z world
  units), `WORLD_SCALE 20.0f` (world units per metre), `TAG_SIZE 0x60`, `TAG_NAME 0x40` (MSVC std::string inside a Tag)
  (melee:28-32). These are HARD-CODED (not found at runtime) despite S:12 "nothing is hard-coded per exe".
- `typedef struct { union { char buf[16]; char *ptr; } u; size_t size, cap; } MsString;` (melee:34) -> `ms_cstr` uses
  `cap > 15` for heap vs SSO (melee:122).
- Typedefs (generic names, collision risk): `GetWeaponFn`, `TagCtorCStrFn`, `ConstGetFn`, `SlotFn`, `MoveCircleFn`,
  `UpdateFn`, `FinishSubFn`, `DtorFn`, `WeaponRangeFn`, `SetTargetFn`, `ClearTargetFn`, `EvalFn` (melee:38-90). Also
  local `SetModeFn` in vs_tick (melee:918). All file-static scope but typedefs are not "static": they only collide if two
  modules are textually in the same TU (unity) - as separate TUs they are fine.
- Offsets found at runtime (`static uint32_t o_*`): `o_weaponry, o_wlistBeg, o_wlistEnd, o_weapItem, o_itemTagBeg,
  o_itemTagEnd` (melee:40); `o_fightOff, o_hStateBrain, o_hBrainEntity` (melee:51-52); `o_vStateBrain, o_vBrainEntity,
  o_vMode, o_vRecBeg, o_vRecEnd, o_recX, o_recY, o_aiOrder` (melee:69); `o_approach, o_recEnt, o_entDef, o_defPropBeg,
  o_defPropEnd` (melee:76-78); `o_entTags` (melee:84); `o_weapOwner` (melee:96); `o_chassis, o_modeSlot` (melee:116).
- Engine fn pointers: `g_getWeapon` (GetCurrentWeapon), `g_tagCtorCStr` / `g_tagCtorForIds` (Tag::Tag(const char*)),
  `g_constGet` (brain const getter, located, never called: grep of every fn-pointer call, R7), `g_vsTickOrig`, `g_moveCircle`, `g_moveCircleOrig`,
  `g_mtUpdateOrig`, `g_mtDtorOrig`, `g_finishSub`, `g_weaponRange`, `g_setTarget`, `g_clearTarget`.
- Code addresses: `g_fdStr, g_fdSiteState, g_fdSiteAttack` (melee:50), `g_vtBattle` (melee:53), `g_hSlot[4], g_hSlotN`
  (melee:54-55), `g_vtVsRoot, g_vtMoveTask` (melee:61), `g_apSite` (melee:68), `g_simTime` (`volatile uint32_t *`,
  sim ms, melee:70), `g_evalFn, g_evalJump` (melee:93), `g_stSites[8], g_stSiteN` (melee:94-95).
- ok flags: `g_okWeapons` (melee:41), `g_okVehicle` (melee:71), `g_okPriority` + `g_wantPriority = 1` (melee:80),
  `g_okHold` + `g_wantHold = 1` (melee:97), `g_okClose` (melee:112).
- Settings with defaults (melee:79,98-117): `g_propMelee[48] = "ai_prioritize_melee"`, `g_propRanged[48] =
  "ai_prioritize_ranged"`, `g_holdFactor = 1.0f`, `g_meleeTag[48] = "melee"` (len 5), `g_meleeTag2[48] = "veh_melee"`
  (len 9), `g_repathFrac 0.25f`, `g_repathMin 1.0*20`, `g_repathMax 10.0*20` (world units), `g_repathMs 500`,
  `g_repathMsClose 200`, `g_closeRange 10*20`, `g_contact 0.5f`, `g_leadSec 0.6f`, `g_leadMax 6*20`, `g_recede 0.3*20`
  (world units per ... see use), `g_pursue 1`, `g_chaseAlways 1`, `g_chaseMode 2`, `g_smooth 1`, `g_logChase 1`.
- `rd_u32` (returns int, signed rel32) and `rel32_target(insn,len)` = insn+len+rel32 at insn+len-4 (melee:124-125).

### Interned tag ids (melee:127-175)
- Comment melee:127-129: Tags interned, equal when first qword (id) equal (engine Tag== at exe+0x556810, a comment
  address, not used). Ids built lazily on the first engine call that needs them, on the engine thread, with the engine's
  Tag(const char*) ctor.
- `g_idMelee, g_idMelee2, g_idPropMelee, g_idPropRanged` (uint64), `volatile LONG g_idState` 0 none / 1 building /
  2 ready / 3 unavailable, `g_tagCtorForIds` (set in find_weapons, melee:309).
- `ids_ready()` (melee:133-157): FX_TEST -> always 0. state 2 -> 1; else if state != 0 -> 0; CAS 0->1 (one thread
  builds; others get 0 = name compare meanwhile); no ctor -> state 3. Builds 4 Tags into `static uint8_t
  buf[4][0xC0]` aligned 16 (never freed, never destructed), names = {meleeTag, propMelee, propRanged, meleeTag2}
  lowercased ASCII (max 47 chars); empty name -> id 0 (melee2 off). id = first qword of the Tag. `MemoryBarrier()`,
  state = 2. NOTE: calls an ENGINE function (Tag ctor; interns the string in the engine's tag table) from inside a
  hook; first call happens on whatever engine thread first reaches a melee/priority test.
- Trust counters (melee:160-175): `volatile LONG g_idChecks, g_idPos`; `IDS_TRUSTED` = `g_idChecks > 400 &&
  g_idPos > 5`. `id_verify(byId, byName, what)`: on disagreement sets `g_idState = 3` (ids off for good, name compares
  from then on) and logs `tag ids disagree with names for "<what>" - using name compares`, returns byName; on agreement
  increments g_idPos if positive, returns byId. g_idChecks incremented by the callers before id_verify.
  Until trusted, BOTH id and name compares run and the name answer wins on disagreement, so the RESULT is always the
  name answer (ids only save time once trusted). Trust state is per process, depends on call count -> different peers
  trust at different moments, but the result does not change unless ids and names disagree after trust (then the id
  answer is used unchecked). Sim-safe as long as id == name semantics (case-insensitive name compare vs ids of the
  lowercased name: an entity tag stored with upper-case letters would have a different id but match by name -> a
  disagreement would be found during the first 400 checks only if such a tag is seen then; inferred risk).
- `tags_have_melee(beg,end)` (melee:178-199): null/neg/size > 256 tags -> 0. id path: any tag id == g_idMelee or
  (g_meleeTag2Len && == g_idMelee2). If trusted return. Name path: size equal and `_strnicmp` with g_meleeTag or
  g_meleeTag2. Then g_idChecks++ and id_verify(..., g_meleeTag).
- `weapon_is_melee(w)` (melee:200-206): w -> `*(w + o_weapItem)` item -> tags_have_melee(item+o_itemTagBeg,
  item+o_itemTagEnd). Null checks on w and item only (no memory probe).
- Priority bits (melee:209-258): PRIO_MELEE 1, PRIO_RANGED 2. `tags_priority_bits_by_name(b,en)` (melee:211-221) name
  compare, `else if` so a tag matching melee name is not tested for ranged (irrelevant, different names).
  `tags_priority_bits(b,en,maxTags)` (melee:222-242): bounds `TAG_SIZE*maxTags`, id path then name path, id_verify
  with what = "ai priority props / tags". `def_priority_bits(e)` (melee:243-247): e+o_entDef -> def ->
  tags_priority_bits(def+o_defPropBeg, def+o_defPropEnd, 256). `def_priority_bits_quiet(e)` (melee:250-257): name only,
  no trust counters (log path; comment says so: logging never moves the id-trust state).
  `bits_to_priority(bits)` (melee:258): melee bit -> 2, ranged -> 1, none -> 0 (melee wins if both).

### Finders part 1
- `find_weapon_layout()` (melee:267-282): scans .text for `48 8B 80 <imm32 item> 00? ...`:
  exact pattern `48 8B 80 [item:4] 48 8B B8 [end:4] 48 8B 98 [beg:4]` with p[5]==0, p[6]==0 (item < 0x10000),
  constraints item <= 0x200, end == beg+8, 0x400 <= beg <= 0x4000. All hits must agree on item and beg, else
  "weapons: held-item tag reads disagree" -> 0. 0 hits -> "weapons: held-item tag read not found". Success log
  `weapons: weapon +0x%X -> item, item tags +0x%X..+0x%X (%d site(s))`. Whole-.text scan; does not look at any
  bytes MS/PF patch (a match inside MatchAmmo / ctx builder prologue is conceivable but the patched prologue bytes are
  E9/FF jumps, which would only REMOVE a hit, inferred).
- `fd_site(lea, &ctor, &getSite, &getFn)` (melee:285-294): first E8 within 0x20 after the lea (+7) = Tag ctor; next E8
  within 0x40 after it = ConstGet; getSite = that E8.
- `find_weapons()` (melee:299-361) ("human:" log prefix kept):
  1. `find_cstr("fight/distance")`; fail "human: \"fight/distance\" not found".
  2. up to 4 `find_lea_to(str,k)`, keep those that are exactly `48 8D 15` (lea rdx); need exactly 2 ("human: expected 2
     readers of \"fight/distance\", found %d").
  3. fd_site each; both must call the same ctor and getter ("human: reader %d not recognised", "human: the two readers
     call different functions"). Sets `g_constGet`, `g_tagCtorCStr`, `g_tagCtorForIds`.
  4. state reader: `F3 0F 11 [modrm mod=10, rm!=4] disp32` (movss [reg+disp32],xmm) within 0x20 after the getter call ->
     `o_fightOff` in [0x100,0x2000]; that site = `g_fdSiteState`, the other = `g_fdSiteAttack`.
  5. `find_vtable_rtti(".?AUeOutdoorsBattle@Human@@")` -> `g_vtBattle`.
  6. first up to 12 slots that are in .text; scan the whole .text for `F3 [REX 4x]? 0F 10 [mod=10,rm!=4] disp32==
     o_fightOff` (movss xmm,[reg+off]) and `func_of` each hit; slots whose function matches -> g_hSlot (max 4, else
     "human: too many slots read the field"; none -> "human: no eOutdoorsBattle slot reads the field").
  7. In those slots (first 0x80 bytes): first `48 8B 49 b` = state->brain, second = brain->entity, then
     `48 8B 89 [W:4] E8` -> o_weaponry = W, g_getWeapon = call target. Bounds 0x400..0x4000 ("human: weapon getter not
     found").
  8. In GetCurrentWeapon first 0x40 bytes: `48 8B 41 L 48 3B 41 L+8` -> o_wlistBeg/End ("human: weapon list not found
     in the weapon getter").
  9. Logs: `human: fight distance field +0x%X, slots`, per slot `  eOutdoorsBattle slot %d = exe+0x%llX`,
     `human: const getter exe+0x%llX, eAttack reader call exe+0x%llX, state->brain +0x%X, brain->entity +0x%X`,
     `weapons: entity +0x%X -> weaponry, current weapon exe+0x%llX, list +0x%X..+0x%X`.
  READ-ONLY since 0.5.0 (comment melee:296-298): nothing found here is patched. Only consumers: g_getWeapon,
  o_weaponry, o_wlist*, g_tagCtorForIds (and the hStateBrain etc. only for logging).
- `find_vehicle()` (melee:363-438):
  1. RTTI vtables `.?AUeVS_Root@Actor@@` (g_vtVsRoot), `.?AUeMoveTask@Order@@` (g_vtMoveTask), `.?AVeSingleOrder@@`
     (vtSingle, local). Fail log `vehicle: vtables not found (eVS_Root %p, eMoveTask %p, eSingleOrder %p)`.
  2. tick = g_vtVsRoot[3] (slot 3) must be in .text (silent `return 0` if not! no log line).
  3. Tick head scan (first 0xC0 bytes, order-independent): `48 8B 49 b` -> o_vStateBrain; `83 B9 [M:4] 01` -> o_vMode
     (cmp dword [rcx+M],1); `48 8B 41 e` -> o_vBrainEntity; `4C 8B A1 [R:4] 4C 3B A1 [R+8:4]` -> o_vRecBeg/End
     (mov r12,[rcx+R]; cmp r12,[rcx+R+8]). Fail "vehicle: tick head not recognised".
     NOTE: this scan reads the FIRST bytes of the tick function, which chase itself later replaces by a VTABLE slot swap
     (not a prologue patch), so the tick's code bytes are never modified by chase. If any other dll put a prologue jmp on
     the eVS_Root tick, `48 8B 49` might be lost (inferred; none of MS/PF patch it per the brief).
  4. `48 B8 'approach'` (movabs rax, 8-byte "approach") anywhere in .text whose func_of == tick rva -> mov. Fail
     "vehicle: \"approach\" not found in the eVS_Root tick". Requires .pdata.
  5. From mov+10 to mov+0xA0: each E8 whose target t is in .text and has within its first 0x40 bytes a
     `48 8D 05 rel32` (lea rax,[rip]) == g_vtMoveTask -> `g_apSite = q` (the call site), `g_moveCircle = t`
     (the eMoveTask(circle) ctor/factory). Fail "vehicle: approach move order call not found".
     -> this is the approach call site that chase later REDIRECTS (rel32). If another dll had already redirected it,
     the rel32 target would be a pad outside the exe .text (alloc_near memory) -> `in_text(t)` false -> not found
     (inferred). Teleport / others: see section 3.
  6. After g_apSite (+5..+0x20): `48 3B 81 [AI:4]` (cmp rax,[rcx+AI]) -> o_aiOrder in [0x40,0x400] ("vehicle: AI
     order slot not found").
  7. Tick first 0x800 bytes: `F3 41 0F 10 44 24 X` then within +7..+0x18 `F3 41 0F 10 4C 24 X+4` -> o_recX = X,
     o_recY = X+4 (movss xmm0,[r12+X]; movss xmm1,[r12+X+4]) ("vehicle: target record position not found").
     Note o_recX/o_recY are 8-bit disp (disp8).
  8. eMoveTask slot 0 in .text and `F6 C3 01` (test bl,1) within its first 0x20 bytes ("vehicle: eMoveTask dtor not
     recognised").
  9. eMoveTask slot 19 = Update: `85 D2 74 xx 83 EA 01` within first 0x40 (test edx,edx; je; sub edx,1) ("vehicle:
     eMoveTask Update not recognised"). Then `33 D2 48 8B 09 E8 xx xx xx xx 48 8B CB E8` within first 0xA0 ->
     g_finishSub = target of the second E8 (f2+13). Not fatal: "vehicle: eMoveTask phase-2 helper not found -
     smooth_repath off".
  10. Sim clock: vtSingle[3] (eSingleOrder::Tick) first 0x40 bytes: `8B 05 rel32 29 43` (mov eax,[rip+QUANT]; sub
      [rbx+..],eax) -> `g_simTime = rel32_target(q,6) - 4` (the dword BEFORE the quant). Must be in .data ("vehicle:
      simulation clock not found"). Cross-check: some `8B 05 rel32 C3` (mov eax,[rip]; ret) in .text targets it
      ("vehicle: simulation clock not confirmed"). Live editor: clock exe+0x13DF988 (= plan's "ed 0x13DF988").
  11. Logs `vehicle: eVS_Root tick exe+0x%llX (state->brain +0x%X, mode +0x%X, brain->entity +0x%X, targets +0x%X, AI
      order +0x%X)` and `vehicle: approach call exe+0x%llX -> eMoveTask(circle) exe+0x%llX, eMoveTask Update
      exe+0x%llX, record pos +0x%X/+0x%X, clock exe+0x%llX`.
- `find_priority()` (melee:444-476) (needs g_vtVsRoot):
  1. In tick first 0x800: `F3 0F 10 81 [A:4] 41 0F 2F` (movss xmm0,[rcx+A]; comiss xmm0,xmm14...) then within +11..+0x20
     `49 8B D4 E8` (mov rdx,r12; call T) -> o_approach = A, tagFn = T ("priority: approach range read not found").
  2. T first 0x20: `48 8B 72 e` (mov rsi,[rdx+e]) -> o_recEnt ("priority: target record entity offset not found").
  3. T first 0x40: `48 8B 86 [DEF:4] 48 8B B8 [END:4] 48 8B 98 [BEG:4]` with END == BEG+8 -> o_entDef, o_defPropBeg,
     o_defPropEnd ("priority: def props read not found").
  4. Target evaluation: whole .text `C7 81 [A:4] 00 00 80 BF` (mov dword [rcx+A], -1.0f) -> ev ("priority: target
     evaluation not found"). First hit only.
  5. ev..ev+0x800: `48 8B CF E8 rel32` followed by either (a) `48 8B 4E x 0F 28 F0` or (b) `0F 28 F0 48 8B 4E` and,
     within 0x80 from q, `F3 0F 11 B6 [A:4]` (movss [rsi+A],xmm6) -> g_weaponRange = call target. Takes the LAST such
     match in the window? No: loop condition `!g_weaponRange` stops at first success ("priority: weapon range getter not
     found").
  6. Log `priority: approach range brain +0x%X, entity +0x%X -> def, props +0x%X..+0x%X, weapon range exe+0x%llX,
     record entity +0x%X`.
  The reset `ev` lies inside the target evaluation; find_hold takes its .pdata function start (same function) and hooks its caller.

## R4b. Raw notes: melee.c part 2 (lines 478-604: finders run mode, hold, entity tags)
- `find_run_mode()` (melee:480-493): RTTI `.?AUeMovementChange@Order@@`, slot 19 in .text ("run mode: eMovementChange
  not found"). First 0x30 bytes: `BF imm32` (mov edi,imm32) with 0x800 <= imm < 0x2000 -> o_chassis. Then `8B 53 3C`
  (mov edx,[rbx+0x3C]) within 0x80, and from it within 0x30 `FF 90 disp32` (call [rax+disp32]) with disp < 0x800 and
  8-aligned -> o_modeSlot (BYTE offset into the chassis vtable). Fail "run mode: chassis / mode setter not recognised".
  Log `run mode: entity +0x%X -> chassis, SetMovementMode = chassis vtable +0x%X`.
  NOTE: o_chassis comes from `mov edi,imm32` = it is an entity offset loaded as a constant (inferred to be used as
  `[rbx+rdi]`); the code trusts the range check only.
- `find_hold()` (melee:499-542): needs o_approach and o_weaponry ("hold: needs the priority and weapon finders").
  1. Same reset pattern as find_priority: `C7 81 [o_approach] 00 00 80 BF` -> ev; `g_evalFn = g_base + func_of(ev)`
     (function START via .pdata) ("hold: target evaluation not found").
  2. Whole .text: every E8 or E9 whose rel32 target == g_evalFn; must be exactly 1 -> `g_evalJump` ("hold: target
     evaluation has %d callers (expected 1)"). Comment says it is a tail jmp (E9). Any byte pair 0xE8/0xE9 counts, not
     instruction-decoded.  CONFLICT NOTE: if another dll already redirected this caller, count = 0 -> hold off.
  3. Within g_evalFn..+0x1400: each E8 whose func_of == evRva (inside the evaluation itself) and target in .text; the
     first target t that has `48 83 B9 [disp32] 00` (cmp qword [rcx+disp32],0) within its first 0x30 bytes AND within
     0x90 `83 7x 08 00 75 xx BA 01 00 00 00 48 8B CB E8` (cmp dword [rsi|rbp..+8],0; jne; mov edx,1; mov rcx,rbx;
     call Clear) -> `g_setTarget = t`, `g_clearTarget` = target of that last E8. Every E8 in the window whose target ==
     g_setTarget is recorded in `g_stSites[]` (max 8, MAX_ST_SITES; more are silently ignored). Fail "hold:
     Weapon::SetTarget not found in the target evaluation".
  4. o_weapOwner: in SetTarget first 0x60: `48 8B [mod=10 rm=1: 81/89/91/...] [OWN:4 in 0x40..0x200] 48 8B [mod=10]
     [0x568:4]` -> OWN. NOTE the literal 0x568 = HARD-CODED weaponry offset (equals o_weaponry on 1.067-1.069 per the
     live log `entity +0x568 -> weaponry`). Fallback: whole .text `48 8B (81|89|..) OWN 48 8B (80..87) [o_weaponry]`
     (rm masked &0xC7==0x80 -> base reg rax only after masking reg field) -> OWN. ("hold: weapon owner offset not
     found").
  5. Log `hold: target evaluation exe+0x%llX (entered from exe+0x%llX), Weapon::SetTarget exe+0x%llX (%d site(s)),
     clear exe+0x%llX, weapon +0x%X -> owner`. Live editor: SetTarget ed 0x6998E0 (4 sites), clear =
     engine weapon cease-fire ed 0x699CC0 (= PenFilter's weapon stop).
- `find_entity_tags()` (melee:554-604) (0.5.4): needs o_vBrainEntity and g_tagCtorCStr ("entity tags: needs the
  vehicle and weapon finders").
  1. find_cstr("no_auto_repair") ("entity tags: \"no_auto_repair\" not found").
  2. Up to 4 static-init sites `48 8D 15 rel32(str) 48 8D 0D rel32(TAG) E8 rel32(g_tagCtorCStr)` -> tags[] = TAG
     addresses ("entity tags: static Tag \"no_auto_repair\" not found").
  3. Every `48 8D 15 rel32` in .text (from text+0x10) whose target is one of tags[]: look in p-0x10..p+0x10 for
     `48 8B 48 E 48 81 C1 C:4` (mov rcx,[rax+E]; add rcx,C) with E == o_vBrainEntity; then the first E8 within 8 bytes
     after max(mv+11, p+7) = HasTag call; HasTag ht in .text; in its first 0x20 bytes two `48 8B (mod10,rm1) disp32`
     loads d0,d1 differing by 8; within 0x40 an `add r64,0x60` (48 83 C0+r 60) stride and an E8 to a function whose
     first 10 bytes are exactly `48 8B 02 48 39 01 0F 94 C0 C3` (Tag==). o = C + min(d0,d1) in [0x100,0x2000). All sites
     must agree ("entity tags: the no_auto_repair tests disagree (+0x%X, +0x%X)"); none -> "entity tags: the
     no_auto_repair test (HasTag on the brain's entity) not recognised". Sets `o_entTags`. Log `entity tags: entity
     +0x%X (no_auto_repair test exe+0x%llX, HasTag exe+0x%llX, %d site(s))`. Comment: 1.067-1.069 = 0x1D0+0x318 = 0x4E8.
     `tests/check_entity_tags.py` = Python replica checked against the {tags} script action's container.

## R4c. Raw notes: melee.c part 3 (lines 606-1062: runtime, all `#ifndef FX_TEST`)
### Chase table (melee:610-624)
- `typedef struct { void *order; uint8_t *ent, *brain; float cx, cy, r, reach; uint32_t next; uint8_t used, started,
  repath; } Chase;` MAX_CHASE 512, `static Chase g_ch[512]`, `g_chHigh` (slots [0,high) may be used), `volatile LONG
  g_chLive` (used count, lock-free fast path), `static CRITICAL_SECTION g_cs` guards g_ch AND g_tr (and the tag-prio
  log list). Key = ORDER POINTER (eMoveTask*). Lowest free slot is taken (melee:904-906).
- `ch_find(o)` linear over [0,g_chHigh) (melee:623). `ch_remove(i)` used=0, g_chLive--, shrink g_chHigh (melee:624).
- Log-once counters `g_regLogged, g_repLogged, g_stopLogged, g_pursueLogged` (melee:616).
- Thread-locals (`_Thread_local`, melee:617-621): `t_apOrder` (order built by the redirected approach call), `t_apX,
  t_apY, t_apR` (circle used), `t_chase, t_haveV` (int), `t_mel, t_vx, t_vy` (float). Set by vs_tick for the unit
  whose tick is running, read by ap_move (same call stack). Plus `t_evalBrain` (melee:1009).
### Target velocity table (melee:627-704)
- `typedef struct { uint8_t *ent; void *tgt; float x, y, vx, vy; uint32_t t; } Track;` MAX_TRACK 1024,
  TRACK_STALE_MS 3000, `static Track g_tr[1024]`. Key = chasing unit's ENTITY POINTER, hashed
  `((uintptr_t)e >> 4) * 2654435761u % 1024` then linear probe. `tgt` = target-record entity pointer (compared only).
- `track(e, rec, &vx, &vy)` (melee:669-704) caller holds g_cs. tgt = rec+o_recEnt, x/y = rec+o_recX/Y, now = *g_simTime.
  Probe all 1024 from h: own slot (ent==e) -> t, stop; never-used (ent==NULL) -> spare if none yet, stop; first slot
  with `now - c->t > 3000` -> spare (keep probing for own). t = own else spare; none -> v = 0 (table full of fresh
  samples). New owner or new target -> reset sample (v=0, t=now). Else dt = now - t->t; if dt >= 100 ms: dt <= 3000 ->
  instantaneous v = dpos*1000/dt, `v = 0.5*v + 0.5*inst` (EMA); dt > 3000 -> v = 0; store x,y,t. dt < 100: unchanged.
  0.5.3 comment (melee:675-682): result must not depend on the slot (heap addresses differ per peer); before 0.5.3 a unit
  pushed out the oldest of 16 slots -> velocity restart on one machine only (seen with ~500 chasing units in an offline
  replay). Slots are only emptied by mission_check, so the own slot always lies before the first never-used one in the
  probe chain (my check: holds, because a never-used slot is never created mid-mission). With > 1024 fresh samples the
  newcomer gets velocity 0 on every machine alike (depends on the count only).
  NOTE: velocity units = world units per second; the 100 ms minimum sample spacing + EMA are sim-clock based.
### Epoch / new mission (melee:644-665)
- `EPOCH_GAP_MS 5000`, `volatile uint32_t g_epochLast` (0 = none), `g_epochLogged`. `epoch_changed(last,now)` =
  `last && (uint32_t)(now - last) > 5000` (catches a step back via unsigned wrap and a jump ahead > 5 s).
- `mission_check()` (melee:648-665): reads *g_simTime; if changed: lock g_cs, re-test, memset g_ch, g_chHigh = 0,
  g_chLive = 0, memset g_tr, g_epochLast = now?now:1, log (max 20, only if g_logChase) `simulation clock %u -> %u ms: new
  mission, chase table cleared (%d slot(s) were still in use)`. Else `g_epochLast = now ? now : 1` (unlocked store).
  Caller must NOT hold g_cs (CRITICAL_SECTION is re-entrant anyway). Called from vs_tick (every vehicle-brain tick) and
  mt_update (only while g_chLive > 0). If no vehicle ticks for > 5 s of sim time while a chase is... no: any 5 s gap
  between two hooked calls clears; comment says some hooked call runs every quant while a chase is live (a vehicle
  brain ticks). A battle with no vehicle-brain units and no live chase: mission_check never runs; first vs_tick of the
  next battle compares against an old g_epochLast -> clears (empty anyway). A pause does not advance the sim clock
  (inferred), so pauses do not clear.
  EDGE: g_epochLast is a single global shared by all threads; written unlocked on the fast path. Determinism argument
  in comment: depends only on the sim clock.
### Geometry helpers (melee:706-741)
- `dist2d` uses `__builtin_sqrtf` (sqrtss, IEEE exact) (melee:706). `lead_point(tx,ty,vx,vy,&px,&py)`: l = |v*leadSec|,
  clamp to g_leadMax by scaling (melee:709-714). `receding(x,y,tx,ty,vx,vy)` = projection of v on the unit vector
  self->target, 0 if d==0 (melee:717-721).
- `unit_reach(e, &ranged)` (melee:724-741): weaponry = *(e+o_weaponry); list [beg,end) (max 64); for each non-null
  weapon: m = weapon_is_melee, r = g_weaponRange(w) (engine call, world units) or -1 if no getter; r > 100000 -> 0;
  melee: mel = max(r) with "-1 when has melee, range unknown" only if mel still 0; ranged: max. Returns mel
  (0 = no melee weapon, -1 = melee with unknown range [only if g_weaponRange is NULL... r<0 when the getter returns
  negative too]). Engine call per weapon per evaluation.
### Hook handlers
- `ap_move(circle, flags)` (melee:743-755) = replacement for the approach call `eMoveTask(circle)`. circle+8 = float
  x, y, radius. If `t_chase && t_mel > 0 && radius <= t_mel + 20` (a melee-reach approach): radius = max(t_mel *
  g_contact, 10 world units = 0.5 m); if t_haveV: center = lead_point(center, t_v). WRITES INTO THE ENGINE'S circle
  struct before calling the original. Then `o = g_moveCircleOrig(circle, flags)`, records t_apOrder/X/Y/R. Returns o.
- Priority (melee:757-828): `unit_priority(e)` (melee:790-802): if o_entTags: bits from entity tag vector
  (e+o_entTags, +8; maxTags 1024); nonzero -> prio (and log-only `log_tag_priority` when g_logChase and caps not hit);
  else def bits. `log_tag_priority` (melee:774-789) caps: TAG_PRIO_LOG_MAX 20 lines, TAG_PRIO_SCAN_MAX 4096 scans;
  `g_tagPrioSeen[20]` {e,prio}; takes g_cs to append; line `unit %p: entity tag "%s" -> %s priority (def props: %s)`.
  `apply_priority(brain,e,rec,mel,rng)` (melee:808-828): ret = unit_priority; none or no rec -> ret; ranged -> ret
  (0.5.4: no write; engine range kept); melee: reach = mel; if !(mel > 0) -> ret; dist = 3D |e+ENT_POS - rec+o_recX|
  (uses rec+o_recX, +4, +8: reads a z at rec+o_recX+8, inferred to be the record's z); want = dist >= reach ? reach :
  -1.0f; if `brain+o_approach != want` -> write (log max 20 `%s unit %p: target %.1f m, approach range %.1f -> %s`).
  WRITES brain+o_approach (engine field).
- `log_no_melee(e)` (melee:832-850) max 15 lines `unit %p approaches but has no weapon tagged "%s" - not chased.
  Weapon tags:%s` (lists weapon item tag names, 24 chars each).
- `vs_tick(s,a,b,c)` (melee:852-925) = eVS_Root vtable slot 3 replacement (the vehicle brain's root state tick).
  1. t_chase = t_haveV = 0, t_mel = 0; mission_check().
  2. brain = *(s+o_vStateBrain); e = *(brain+o_vBrainEntity).
  3. If e && !(e[0x30] & 4) (not human) && (e[0x30] & 2) (alive): mel = unit_reach(e,&rng); rec = first target record
     if `*(int32*)(brain+o_vMode) == 1` and the record vector is non-empty; prio = g_okPriority ? apply_priority(...) : 0.
     If mel != 0: t_chase = 1, t_mel = mel; if rec && g_okClose: lock, track(), unlock; t_v = v, t_haveV = 1;
     meleeUnit = prio == 2 || (prio == 0 && !(rng > 0)); if prio != 1 && mel > 0 && *(brain+o_approach) < 0
     (engine says "no approach: in range"): d = dist2d, back = receding; always = g_chaseAlways && meleeUnit && d >
     mel*0.9; retreat = g_pursue && d > mel*g_contact && back > g_recede; if either: WRITE brain+o_approach = mel (log
     max 20 `melee unit %p: target %.1f m (%s %.1f m/s) inside weapon range - chasing`).
  4. t_apOrder = NULL; r = original tick; o = t_apOrder (set by ap_move if the tick built an approach).
  5. If no o, or brain's AI order (brain+o_aiOrder) != o -> return r. If !t_chase -> log_no_melee, return. If radius not
     in (0, 40000] -> return. self = dist2d(e pos, circle center).
  6. Lock: find slot by order pointer, else lowest free slot (g_chHigh/g_chLive updated); store Chase{o, e, brain, X, Y,
     R, reach = t_mel>0 ? t_mel : R, next = sim + (self < closeRange ? 200 : 500), used 1, started 0, repath 0}. Unlock.
     Table full (512) -> silently not stored.
  7. If o_modeSlot && g_chaseMode >= 0: chassis = *(e+o_chassis); call chassis vtable[o_modeSlot/8](chassis,
     g_chaseMode, 0) = SetMovementMode (ENGINE CALL changing sim state: run speed).
  8. Log max 20 `melee unit %p approaches (%.1f m away, goal radius %.1f m) - following the target`.
- `mt_update(o, phase, a, b)` (melee:927-994) = eMoveTask vtable slot 19 replacement, runs for EVERY eMoveTask of every
  unit (infantry included), every quant while the order runs. Fast path: g_chLive == 0 -> original (no lock). Else
  mission_check; recheck; lock; ch_find(o) (linear) miss -> original.
  - phase 0 (start): if started already -> ch_remove (stale reused pointer), else started = 1; original.
  - phase != 1 (stop, i.e. 2): smooth = repath && g_smooth && g_finishSub; if not and g_smooth && g_finishSub && g_okClose
    && mode == 1 && first record non-null: track(); smooth = g_pursue && receding > g_recede. ch_remove; unlock; if smooth:
    `g_finishSub(o); return 1` (skips the chassis stop of phase 2; "drop the path, keep the chassis rolling"); else
    original.
  - phase 1 (running): now = sim; if now < next -> original. Copy slot; rec = first record (mode == 1); track if
    rec && g_okClose; self = dist2d; next = now + (self < closeRange ? 200 : 500); unlock. No rec -> original. Lead point,
    moved = |lead - (cx,cy)|, back = receding. If `self <= reach*0.9 && back <= g_recede` -> log max 20 `melee unit %p:
    target in reach (%.1f m) - stopping`, return 1 (does NOT call the original: inferred, return value 1 = order finished,
    the engine then runs phase 2 / deletes it). Threshold thr = clamp(self*frac, min, max); moved > thr -> lock, set
    repath = 1, unlock, log max 30 `melee unit %p: target goal moved %.1f m - new path (%.1f m away, %s %.1f m/s)`,
    return 1 (finish -> vs_tick re-approaches with the new point). Else original.
  NOTE: in phase 1 the code reads brain fields of `ch.brain` (stored pointer) - lifetime assumption: the brain outlives
  its order (inferred; mission_check + dtor hook guard reuse).
  NOTE: `ch.reach` is the reach stored at registration (t_mel or R).
- `mt_dtor(o, flags)` (melee:998-1006) = eMoveTask slot 0 (scalar deleting dtor) replacement: if g_chLive: lock,
  ch_find, remove. Then original.
- Hold (melee:1009-1043): `_Thread_local uint8_t *t_evalBrain`, `g_evalOrig`, `g_setTargetOrig`, `g_holdLogged`.
  `eval_wrap(brain,a,b,c)` (melee:1014-1021): save/restore t_evalBrain around the original evaluation (re-entrant
  safe). `st_wrap(w, desc, a, mode, b)` (melee:1023-1043): brain = t_evalBrain; if brain, w, desc and `desc+8` (eTarget
  type) != 0 and the weapon is NOT melee: e = brain's entity; if e == weapon owner (w+o_weapOwner), not human, alive,
  mode == 1: mel = unit_reach (if first record non-null); if mel > 0 and not ranged priority (only when g_okPriority):
  2D distance to the FIRST target record <= mel * g_holdFactor -> log max 10 `melee unit %p: target in melee reach -
  ranged weapon holds fire` and `return g_clearTarget(w, 1)` (the engine weapon cease-fire, instead of SetTarget).
  Else original SetTarget. NOTE: compares with the brain's first target record, not with `desc`'s target.
- `install_hold()` (melee:1045-1051): redirect_call(g_evalJump -> eval_wrap, orig g_evalOrig, "target evaluation");
  then for each SetTarget site redirect_call(site -> st_wrap, orig g_setTargetOrig, "weapon target") (each site gets its
  own 64 KB-granule pad); if site 0 fails return 0 (eval_wrap stays installed), if a later one fails return 1 (partial).
- `install_vehicle()` (melee:1053-1061): InitializeCriticalSection(&g_cs); vt_hook eMoveTask slot 0 (mt_dtor), slot 19
  (mt_update), redirect_call(g_apSite -> ap_move, orig g_moveCircleOrig, "approach move order"), vt_hook eVS_Root slot 3
  (vs_tick). Any failure returns 0 but the earlier hooks STAY (no rollback); they are pass-through while nothing registers
  (vs_tick never installed -> no chase ever registered -> g_chLive stays 0).

## R5. Raw notes: crashlog.c (115 lines, all `#ifndef FX_TEST`), main.c (132), test_main.c (39)
### crashlog.c
- `static HMODULE g_self; static volatile LONG g_vehCount;` (crash:11). g_self also set in init (main:18).
- `addr_desc(a,out,n)` (crash:13-21): GetModuleHandleExW(FROM_ADDRESS|UNCHANGED_REFCOUNT) -> `"<module file>+0x%llX"` or
  `"0x%llX (no module)"`.
- `log_stack(rsp,max)` (crash:23-35): up to 0x400 qwords read with ReadProcessMemory (safe probe), prints the ones that
  are inside the exe .text or inside g_self, `"  [rsp+0x%03X] %s"`, max lines.
- Exit trace (crash:37-89): `ExitProcessFn` / `TerminateProcessFn` typedefs, `g_exitProcessOrig`,
  `g_terminateProcessOrig`; `hook_exit_process` logs `ExitProcess(%u) called from %s` + stack 16; `hook_terminate_process`
  logs only for self (`TerminateProcess(self, %u) called from %s`). `iat_patch(mod,target,hook)` rewrites every IAT entry
  of `mod` whose value == target (VirtualProtect RW per entry). `install_exit_hooks()` resolves kernel32
  ExitProcess/TerminateProcess with GetProcAddress and patches the IATs of the exe, `BugSplat64.dll`, `bdcap64.dll`; logs
  `exit trace                     %d import(s) hooked (ExitProcess/TerminateProcess)`.
  LIVE: editor log line 3 says `0 import(s) hooked` - GEM Fauna (gem_fauna.dll, loads earlier: names sort before) already
  rewrote those IAT entries (`[GEM Fauna] exit trace 4 import(s) hooked`), so the entry no longer equals kernel32's
  address; GEM ScriptExt (newer copy of the same code) prints `done by another DLL loaded earlier (gem_fauna.dll+0x10F50)`.
  So in today's payload MeleeChase's exit trace is dead code; the exits are traced by Fauna.
- `fx_veh(ep)` (crash:91-113): vectored handler (added FIRST, `AddVectoredExceptionHandler(1, ...)`, main:31). C++ throw
  0xE06D7363: first 6 logged (`C++ exception thrown (thread %lu)` + 10-line stack), CONTINUE_SEARCH. Fatal codes
  C0000005, C000001D, C0000096, C00000FD, C0000409, C0000374, C0000094, C000008C: first 8 logged
  (`EXCEPTION 0x%08lX at %s (%s 0x%llX) thread %lu` with read/write + address for AV, regs line `  rax=.. rcx=.. rdx=..
  r8=.. r9=.. rsp=..`, 16-line stack). Never handles anything (always CONTINUE_SEARCH). First-chance: an AV the engine
  catches itself (SEH) is logged too. Counters are per dll.
### main.c
- `ini_float(key,def,lo,hi)` (main:7-14): `%g` default -> ini_str -> strtof; unparsable or NaN -> def; clamp [lo,hi].
- `init(self)` (main:16-120), called from DllMain PROCESS_ATTACH (under the loader lock; everything below runs there):
  1. g_self; g_dir = folder of this dll (GetModuleFileNameW(self), cut at last '\').
  2. Exe gate (main:21-23): `_wcsnicmp(<exe file name>, L"call_to_arms", 12)` != 0 -> return silently (no log file is
     even opened). So call_to_arms.exe, call_to_arms_ed.exe, call_to_arms_server.exe AND call_to_arms_workshop.exe (exists
     in binaries\x64) pass the gate (workshop tool: finders would fail and log; inferred, the launcher may never inject it).
  3. `enabled` (default 1) == 0 -> return silently (main:24).
  4. `log` (1), `console` (0), `verbose` (1, read into g_verbose which NOTHING uses). Log = `_wfopen(g_dir +
     "\gem_meleechase.log", L"w")` (truncated every start; ONE name for game, editor and server: unlike MS / PF there is
     no `_editor` / `_server` suffix, so a hosted game's server process and the game process write the same file when
     they share a payload folder - inferred risk). console -> AllocConsole + freopen CONOUT$.
  5. `get_sections()` fail -> log `not a PE I understand`, return.
  6. Banner `GEM MeleeChase 0.5.4 loaded into %ls (timestamp %08X)`.
  7. `crash_log` (1): AddVectoredExceptionHandler(1, fx_veh) -> `crash log                      ON (fatal exceptions
     are written here)`; install_exit_hooks().
  8. Settings (main:35-67): melee_tag (empty -> log `melee_tag is empty - nothing to do`, return), vehicle_melee_tag,
     vehicle_chase, repath_min_m, repath_max_m (max >= min enforced), repath_fraction, repath_interval_ms [100,10000],
     repath_interval_close_ms [50,10000], close_range_m, contact_factor, lead_seconds, lead_max_m, pursue_retreating,
     chase_always, chase_run_mode (> 7 -> 2; negative kept = off), hold_ranged_in_melee, hold_ranged_reach_factor,
     retreat_speed_mps, smooth_repath, log_chase, weapon_priority, prop_prioritize_melee, prop_prioritize_ranged.
     (Full table in section 6.)
  9. `g_okWeapons = find_weapon_layout()`; fail -> `melee weapon test               OFF (engine code not recognised) -
     nothing to do`, return (everything off). Logs the melee words.
  10. `humans                         not touched (0.5.0: ...)`.
  11. Vehicle block (main:78-118): wantVehicle 0 -> `vehicle melee chase            off (ini)`. Else `!o_wlistBeg &&
      !find_weapons()` -> `vehicle melee chase            OFF (weapon list not found)` (o_wlistBeg is always 0 here, so
      find_weapons always runs). Else:
      found = find_vehicle(); prio = found && find_priority(); g_okClose = prio; g_okPriority = prio && g_wantPriority;
      prioTags = g_okPriority && find_entity_tags(); if found && g_chaseMode >= 0 && !find_run_mode() -> o_modeSlot = 0;
      g_okVehicle = found && install_vehicle(); if !g_okVehicle -> g_okPriority = g_okClose = 0 (note: install_vehicle
      may have left some hooks in place; they are pass-through). Status lines (vehicle melee chase ON/OFF, melee close
      combat ON/OFF, melee chase always), then `if g_okVehicle && g_okClose && g_wantHold: g_okHold = find_hold() &&
      install_hold()` (hold installs AFTER the vehicle hooks), chase run mode line, weapon priority lines.
  12. `simulation change: every player of a multiplayer match needs this DLL` when g_okVehicle.
  GATE CONSEQUENCES: find_weapon_layout fail = whole dll off. find_weapons fail = vehicle part off. find_vehicle fail =
  vehicle, close, priority, hold off. find_priority fail = close + priority + hold off (chase re-plan alone still runs:
  t_mel > 0 needs g_weaponRange... NO: unit_reach uses g_weaponRange ? ... : -1 -> mel = -1 for melee units, so
  `mel != 0` -> t_chase = 1 but `t_mel > 0` false: ap_move does not shrink the circle; vs_tick registers the approach;
  mt_update re-plans with reach = t_apR). find_entity_tags fail = def props only. find_run_mode fail = no SetMovementMode.
  find_hold fail = hold off. g_finishSub missing = no smooth re-path.
- `DllMain` (main:122-131): PROCESS_ATTACH -> DisableThreadLibraryCalls, init; PROCESS_DETACH with reserved == NULL
  (FreeLibrary) -> `DLL detach (FreeLibrary)`. Returns TRUE always (even when off). NO threads are started anywhere in
  the module (no CreateThread / _beginthread; grep). No unhooking at detach.
### test_main.c (FX_TEST only)
- `int main(argc, argv)` (test:7-38): reads the exe file, `mmap((void*)ImageBase, SizeOfImage, RW, MAP_PRIVATE |
  MAP_ANONYMOUS | MAP_FIXED_NOREPLACE)` (Linux only), copies each section's raw bytes (min(raw, virtual)) to base+VA,
  sets g_text/g_rdata/g_data/g_pdata, prints `<path> timestamp %08X`, runs find_weapon_layout, find_weapons,
  find_vehicle, find_priority, find_run_mode, find_hold, find_entity_tags (all unconditionally, `ok &=`), prints
  `smooth re-path helper: found|not found`, `ALL FOUND` / `FAILED`, returns !ok. No relocations: the finders compare
  absolute pointers (find_vtable_rtti's COL pointer `*(uint8_t**)(rdata+j) == col`, vtable slot values used as code
  addresses), so the image MUST sit at its preferred ImageBase (0x140000000 for all three exes) or be relocated.
  Nothing is faked: no engine memory, no hooks (patch.c / runtime part of melee.c / crashlog / main are #ifndef FX_TEST);
  `ids_ready()` returns 0 under FX_TEST (melee:135-136). There are NO behaviour cases (no chase/priority/hold logic test).

## R6. Raw notes: build.bat, tests\, ini, README, live log
- build.bat (42 lines): finds zig (`zig\zig.exe`, `..\..\zig\zig.exe`, ScriptExt's, PATH, `python -m ziglang`), compiles
  `zig cc -target x86_64-windows-gnu -O2 -s -shared -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers
  -o dist\gem_meleechase.dll src\gem_meleechase.c -lkernel32 -luser32` (build.bat:21-22). No -mcpu, no
  -ffp-contract, no -fno-fast-math. Deletes dist\*.lib, copies the ini. `payload` arg copies dll + ini to
  `..\..\launcher\payload\`. No test build in build.bat.
- tests\run_offline_check.sh (14 lines): Linux/WSL: `gcc -O1 -DFX_TEST -Wall -Wextra -Wno-unused-parameter
  -Wno-unused-function -Wno-unused-variable -o out/mc_test src/gem_meleechase.c`, runs it per exe; then `clang
  -fsyntax-only -target x86_64-windows-gnu -fshort-wchar -nostdinc -I tests/stub ... -Werror` syntax check of the real
  Windows build against the fake headers in tests\stub (windows.h 82 lines "for gem_fauna.c", tlhelp32.h, stdint.h for
  LLP64).
- tests\check_entity_tags.py (269 lines, needs pefile): route 1 = find_entity_tags re-implemented; route 2 = the {tags}
  script action's container offset (`add rdx,0x1D0`) + `mov rbx,[rdx+0x318]` (as SensorTags / TeamTags find it); passes
  when both agree and route 1's Tag ctor equals the "fight/distance" ctor. README:257 says `ALL OK`, +0x4E8 on all three.
- gem_meleechase.ini (84 lines): identical to dist\ and to the live payload a1d6ed3c ini (diff empty). Section
  `[meleechase]`. Values = code defaults for every key (section 6 table).
- README.md (257 lines): 0.5.4 status line says "not run in game yet" (README:8) - the live editor log of today is the first
  run. STALE lines: README:145 and README:157 mention a "ranged fallback ON (... < 80% of the ranged reach)" startup line
  and a `ranged-priority unit ...: usable weapons reach X m < ranged reach Y m -> melee priority` play line; NO such code
  exists in the 0.5.4 source (grep "fallback": only melee.c:533, the weapon-owner fallback) and the live log has no such
  line. README "Engine facts" are 1.067 game addresses (README:173), stale but marked as re-derived. README:111 "weapon
  +0x90 is the owner entity" matches 1.069.
- Live editor log `inputs\live_payload_a1d6ed3c\gem_meleechase.log` (91 lines, run 12:16-12:36): every finder line equals
  the probe output for call_to_arms_ed.exe 6AFC6E14 (R7). Play lines: one melee-priority unit (dread test) at 12:27:33:
  `approach range -1.0 -> melee reach`, `approaches (18.9 m away, goal radius 3.0 m)`; then 12:27:38-12:27:44 repeated
  `target in melee reach - ranged weapon holds fire` (10 lines, cap), `target 5.8 m (moving away at -0.0 m/s) inside weapon
  range - chasing` (20, cap) and `approaches (5.8 m away, goal radius 3.0 m)` about every 20-25 ms (12:27:42.965-43.153,
  44.557-44.643): the approach is re-issued every few quants at a constant 5.8 m (unit not closing). Observation only;
  cause not established from the source (candidates, inferred: the circle of radius 3.0 m (= 0.5 x 6 m reach) cannot be
  entered, the move ends at once and vs_tick re-approaches; or the AI order is replaced each tick). `new mission` lines at
  12:28:41, 12:30:42, 12:32:46, 12:36:18 (editor mission restarts; clock stepped back) - epoch reset works.

## R7. Probe results (this map's own Windows port of test_main.c, run Nov 20 2026 on the installed 1.069 exes)
- Files (mine, outside inputs\): `probe\chase\chase_probe.c` (+ .exe, `out_<exe>.txt`), `probe\chase\pe_info.py`,
  `probe\chase\cmp_sections.py`, `probe\chase\rb\` (rebuilds). chase_probe.c #includes the snapshot's core.c + melee.c
  UNCHANGED under `#define FX_TEST 1` with `<windows.h>` (no sys/mman.h), maps the exe with
  `VirtualAlloc((void*)ImageBase, SizeOfImage, MEM_RESERVE|MEM_COMMIT, PAGE_READWRITE)` (fallback: any address + apply
  the .reloc DIR64 entries), copies headers + sections. It mapped all three at the preferred base 0x140000000 (the zig
  test exe itself is ASLR-relocated elsewhere). Compiled with the merged flags, no warnings with -Wall -Wextra
  (-Wno-unused-*).
- All finders found on game 6AFC6D57, editor 6AFC6E14, server 6AFC6DAF (`ALL FOUND`); editor values == live log exactly.
- Rebuild check: `zig cc` 0.14.0 (C:\Users\alex_\Documents\IndomitusLauncher\zig) with build.bat's exact flags on the
  snapshot reproduces dist\gem_meleechase.dll = live payload dll BYTE FOR BYTE (sha256 e992118a...; `rb\asis`). Adding
  `-mcpu=baseline` alone: identical. `-fno-fast-math` alone: identical. `-ffp-contract=off` alone: .text changes (0x98D6 ->
  0x98C6 bytes). Asm diff (`zig cc -S`): only float code at melee.c:706 (dist2d), 719 (receding), 818-819 (apply_priority
  3D distance), 948 and 968 (mt_update): with contraction off the SLP vectorizer packs the x/y lanes into mulps/subps
  (+ shufps/unpcklps) instead of scalar mulss/subss; the number of sqrtss (7), divss (2), ucomiss (3) is unchanged. Packed
  SSE lanes round exactly like the scalar ops, the only reordering is the commutative a+b -> b+a, and LLVM never
  reassociates without fast-math flags, so every float result is bit-identical (inferred from IEEE semantics + the asm
  diff; no FMA instruction exists at baseline x86-64 either way, zig defaults to baseline for an explicit -target).
- Other payload dlls loaded BEFORE chase that touch the same engine area (editor live logs): GEM Fauna names call sites
  exe+0x82F1C2 (root re-target) and exe+0x82F430 (crush condition) (inferred hooked; Fauna source not read) - both below the eVS_Root tick window
  (0x82F700..0x82FF00), and calls the move-action movement change exe+0x788C00; Fauna's Tag(cstr) = exe+0x3E44D0 = chase's.
  Fauna also hooks the chassis movement-mode machinery (`sprint: eChassisFoot vt ... SetMovementMode slot 45`) - chase
  CALLS chassis vtable +0x168 = slot 45 (SetMovementMode) for vehicle-brain units; if those are foot-chassis fauna,
  the call goes through whatever Fauna installed (inferred; not read in Fauna's source).
