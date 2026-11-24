# GEM Melee merge: requirements from the notes (map, Nov 20 2026)

What the merged `gem_melee.dll` (MeleeStance 0.7.42 + MeleeChase 0.5.4 + PenFilter 0.1.1) must keep, what Alex decided,
what went wrong before, what is still open, and what the mod expects from it. Read-only work: nothing outside
`_tools\gem_melee\design\` was written. The mod scan script is `design\req_scan\scan_mod.py` (output `scan_out.txt`, run
Nov 20 2026, 10153 text files of `resource\set|properties|map|script|interface\action` + `entity\*.def|ext|inc|set`).

## Citation keys
| Key | File |
|---|---|
| DH:n | `_notes\dll_hooks.md` line n (line 170 is one ~20000-char line holding MeleeStance 0.7.1-0.7.27: cited as `DH:170 (0.7.x)`) |
| KB:n | `_notes\known_bugs.md` |
| DR:n | `_notes\dreadnought.md` |
| LN:n | `_notes\launcher.md` |
| ID:n | `_notes\infantry_damage.md` |
| FP:n | `_notes\penfilter_flanking_plan.md` |
| HE:n | `_notes\he_vs_vehicles_proposal.md` |
| CQ:n | `_notes\conquest.md` |
| BR:n | `_notes\melee_dll_merge_brief.md` (fact brief written by another session at 13:25 today; used as a cross-check) |
| PL:n | `_notes\gem_melee_merge_plan.md` |
| MSR / MCR / PFR:n | `inputs\meleestance_0.7.42\README.md`, `inputs\meleechase_0.5.4\README.md`, `inputs\penfilter_0.1.1\README.md` |
| ms.c:n | `inputs\meleestance_0.7.42\src\gem_meleestance.c` |
| mc:n | `inputs\meleechase_0.5.4\src\mc\melee.c` |
| pf.c:n | `inputs\penfilter_0.1.1\gem_penfilter.c` |
| MSlog / MClog / PFlog:n | `inputs\live_payload_a1d6ed3c\gem_meleestance_editor.log` / `gem_meleechase.log` / `gem_penfilter_editor.log` (Alex's editor runs of Nov 20, 12:16-12:36) |
| mod paths | relative to `mods\Indomitus_DEV\resource\` |

---

## 1. MUST NOT REGRESS

### 1.1 Seen working by Alex (editor or game)

**Stance module (MeleeStance)**

| # | Behaviour | Version, date | Alex's words / evidence | Source |
|---|---|---|---|---|
| NR-S1 | Stance unit refuses bolt pistols / grenades, picks heavy melee ammo; charge orders bring units in from 634-841 units | 0.2, Nov 6, editor | "works well" | DH:21 |
| NR-S2 | Toggle with 5 selected -> swords out -> run in -> melee (multi-unit `{apply_to_all}` toggle, take-item draw, eMovementChange(RunMode 2)+eMoveTask run beyond CloseDistance 200, `eFight` inside) | 0.6.10, Nov 7, editor | "toggle -> swords out -> run in -> melee" | DH:49, DH:169 |
| NR-S3 | Toggle popups correct on all 5 (Off on all, then On on all) | 0.7.2-era video, Nov 6 | video | DH:37 |
| NR-S4 | Auto-draw of the melee weapon in fight range (AI) | 0.7.0, Nov 7, editor | "taking out melee weapon in cqc works great" | DH:55 |
| NR-S5 | Stance squads vs vehicles: press against the hull and swing (stall / range shift), kill guardsmen + cannon, Russ "killed fast" | 0.7.4-0.7.6, Nov 7 | "worked pretty well"; "slaughtered the nearby guardsmen and cannon" | DH:59, DH:60 |
| NR-S6 | Infantry, cannons, vehicles, weapon swaps (PUT AWAY -> IN HAND in 0.3-0.9 s) | 0.7.10, Nov 8, editor | "looks good" (= "the current working build") | DH:63, DH:64, DH:170 (0.7.10) |
| NR-S7 | Units under direct control left alone (no stance filter, charge, auto-draw, reload cancel) | 0.7.11, Nov 8, editor | "Editor-tested OK" | DH:65, MSR:247-253 |
| NR-S8 | Fight-range switch both ways under a right-click attack order: reload cancel -> take-item -> knife in hand ~0.5 s -> eFight; gun (`sm_bolter`) taken back when nobody is in range; jumping assault marines skipped (`tp_jetpack`); tick 13-25 us average | 0.7.25 / 0.7.26, Nov 9 02:38 / 02:49, game | "the fight-range switch WORKED"; "both directions work by the log" | DH:170 (0.7.25, 0.7.26) |
| NR-S9 | Local 2v2 (no server exe) played to the end, clean exit, MeleeStance 9500 ticks at 29 us | 0.7.32, Nov 10 | Alex: UserControl / jetpacks / CostRed worked; log clean | DH:95 |
| NR-S10 | Dedicated server survives match end (`clock 982304 -> 156: new mission ... unit tables cleared`, exit code 0) | 0.7.32, Nov 10, hosted 2v2 | server log | DH:97 |
| NR-S11 | Hosted 3v3 on the dedicated server | 0.7.41, Nov 15 | "meleestance worked well"; match "played a lot better, less spam and more fluid" | DH:123 |
| NR-S12 | Engine paired melee through forcefields (ray patch) | 0.7, Nov 6, editor | "works" - but Alex then turned it OFF (0.6.1 default `[ShieldMelee] Enabled=0`, decision D-S5). Keep the code path, keep it off. | DH:27, DH:38 |

**Chase module (MeleeChase)**

| # | Behaviour | Version, date | Evidence | Source |
|---|---|---|---|---|
| NR-C1 | Editor run with PenFilter 0.1.1 + MeleeChase 0.5.4 (payload a1d6ed3c), Black Legion tactical squad, no twitching | 0.5.4, Nov 20 12:28 | "seems to be working now with this launcher" | DH:178, LN:183, KB:44 |
| NR-C2 | Assault-cannon dread walks up to a Russ (approach + entity-tag / prop priority). The flame-instead-of-punch part was ammo.set, fixed in data | 0.5.4, Nov 20 | Alex's test | DR:247; MClog:27-77 (melee-priority unit approaching from 18.9 m) |
| NR-C3 | Bot-ordered fauna are not touched (chase only inside the brain's own approach tick) | 0.5.2, Nov 15 | "doesn't change the behaviour of units under control of bot Lua" (observation; server log agrees) | CQ:22 |
| NR-C4 | Humans untouched (0.5.0): log `humans not touched (0.5.0: ...)` | 0.5.0+, Nov 9 | Alex asked for it ("remove the human stuff from meleechase so it's only fauna") | LN:143-147, MClog:7 |

No in-game "works" quote exists for the chase itself (re-path, lead, run mode, hold) in the notes; the README describes the
behaviour and the 0.2.0 fauna test only as a log reading (MCR:52-53). Treat MCR:29-112 as the behaviour to preserve.

**Pen module (PenFilter)**

| # | Behaviour | Version, date | Evidence | Source |
|---|---|---|---|---|
| NR-P1 | First editor run of 0.1.1 judged OK (with MeleeChase 0.5.4) | 0.1.1, Nov 20 12:28 | "judged OK by Alex" | DH:178 |
| NR-P2 | No AP / HE flip and no repeated reload starts on the Russ vs the dread (0.1 showed both; 0.1.1 verdict hold + blocked-volume cache fixed it) | 0.1 (tested by mistake), Nov 20 | fixes "from the first in-game test (Russ AP / HE flip, picks running out)" | PFR:14, PFR:34-38, PFR:224-226, LN:182 |
| NR-P3 | Aim priority body > turret > aux turrets > arms > legs > tracks (P lines in the live log: group 0 kept, falls to group 3 when body blocked) | 0.1.1, Nov 20 | PFlog:20-38 | DH:176, PFR:16-21 |

**Cross-dll / payload**

| # | Behaviour | Evidence |
|---|---|---|
| NR-X1 | GEM Teleport registers on `gem_meleestance.dll!gem_quant_listen` and gets one tick per quant (jetpack aim rings) | Teleport 0.2.7 confirmed working with MeleeStance 0.7.24+ (LN:104); live MSlog:347 `quant listeners: callback ... registered (slot 1 of 4); live now` |
| NR-X2 | MeleeStance consumes Teleport's `gem_order_listen` (order events put units on the poll list) | LN:108 (25 order events in 24 s), MSlog: `orders: listening to GEM Teleport's GiveOrder hook (gem_order_listen)` |
| NR-X3 | Jetpacks work in hosted MP (client view on the dedicated server) with MeleeStance's quant hook in the payload | DH:88 |
| NR-X4 | Multi-select runs every `{apply_to_all}` + `{instant}` button once per eligible unit. Users today (grep): `interface\action\activate_drop.set`, `aura_ig_commissar.set`, `aura_sm_chaplain.set`, `jetpack.set`, `krieg_foxhole.set`, `krieg_pillbox.set`, `melee_toggle.set`, `overcharge.set`, `overcharge_vehicle.set` | DH:24, MSR:124-127, scan |

### 1.2 Built, never seen working in game (keep, but they are not proven)
- Stance: 0.7.42 `AutoMeleeTag` no-melee rule (DH:179); 0.7.33 / 0.7.37 weak-ref fixes (crash classes that cannot be provoked on demand; KB:28, DH:103); `[Zerg]` 80 m charge for `melee_infantry` (DH:83-84; no explicit in-game quote, the 0.7.41 3v3 had bots, DH:123); 0.7.39-0.7.41 player-order redirect / PlayerMaxDistance / ghost-record test (DH:120-122; only "worked well" overall, DH:123); 0.7.12 manager test (DH:66); `hull_contact` tag path (never fired, DH:59); 0.7.35 ClientGate / FastPass (DH:98; the 0.7.40 entry says "performance better", DH:121).
- Chase: 0.5.4 entity-tag priority (`dread_limbs` sets `ai_prioritize_melee`), ranged priority leaving the approach range to the engine (DH:174, DR:238); 0.5.3 address-independent velocity table (MCR:163-171); no 0.5.4 run on the game or server exe (BR:449, 457).
- Pen: everything in a battle beyond the one editor run; hook X weapon stop never fired (`weapons stopped 0` in PFlog:620); never on a dedicated server or in MP (BR:572); hook C (approach) not built (PFR:172).

---

## 2. Alex's decisions (do not "fix" these away)

| # | Date | Decision | Module | Source |
|---|---|---|---|---|
| D-S1 | Nov 6 | Dawn of War style toggle; tag-driven (`melee_stance`); no mission triggers: the charge lives in the dll | stance | DH:21 |
| D-S2 | Nov 6 | Blow reach: tried 40/45/50, 75 = too far, 50; after the rollback 40/40/40; Nov 7 45/45/45 (current ini) | stance | DH:23, DH:36, DH:42, DH:44 |
| D-S3 | Nov 6 | AI ignores downed units (`start_personage`): nothing fires at or charges them | stance | DH:28, DH:35 |
| D-S4 | Nov 6 | Rollback: no shield damage from blows, only the rosarius fx / sound on hits; keep the downed ignore | stance (+script) | DH:35 |
| D-S5 | Nov 6 | "0.6 worked best": `[ShieldMelee]` default Enabled=0 (engine paired melee does not start on forcefield wearers) | stance | DH:38, CLAUDE.md Forcefields |
| D-S6 | Nov 6 | Clicking the toggle takes the melee weapon out; then the double-click attack order is melee | stance | DH:40 |
| D-S7 | Nov 7 | Beyond 200 units the double-click run, inside 200 the melee order (`eFight`), not the attack order | stance | DH:48 |
| D-S8 | Nov 7 | Units in fight range draw melee; stance charge also goes after vehicles, infantry first unless ordered | stance | DH:54 |
| D-S9 | Nov 7 | Stance units "should just attack [the tank] until it is dead" (no re-orders around a vehicle) | stance | DH:56 |
| D-S10 | Nov 7 | Hull: "target the body volume instead of the centre?" -> range measured from the unit when pressed; "stop when they hit the ram volume, then swing" | stance (+script) | DH:57, DH:58 |
| D-S11 | Nov 8 | Direct control: no melee behaviour and no CQC weapon pull-out for the unit the player drives | stance | DH:65 |
| D-S12 | Nov 8 | Moving the melee rows above the gun rows in ammo.set is rejected (units would pull melee instead of guns) | data / stance | DH:170 (0.7.15), MSR:286-287 |
| D-S13 | Nov 9 | Fight range = melee + the fight order; "always pull out a melee weapon if possible when ordered to attack within fight range or when the AI is using them", heavy weapons too | stance | DH:170 (0.7.16, 0.7.17) |
| D-S14 | Nov 9 | "put them in that stance when they are in fight range, then back to range stance when no target is in fight range"; `NoMeleeMs` 20000; `LeaveFactor` 1.5 | stance | DH:170 (0.7.24, 0.7.25, 0.7.27) |
| D-S15 | Nov 9 | `SkipTags` / `IgnoreTargetTags` lists; a sim-only gate was built and dropped the same day | stance | DH:82 |
| D-S16 | Nov 9 | Replace the `melee_inf_atk` mission triggers: "charge from 80m, I don't want the lua logic getting them pinned down"; then only for units tagged `melee_infantry` (0.7.29 "would take over units that pull swords out in combat") | stance | DH:83, DH:84 |
| D-S17 | Nov 10-11 | Cost work "do 1-4" (tick-only flushes, FastPass, known-units cache, ClientGate), no behaviour removed | stance | DH:98 |
| D-S18 | Nov 11 | `[AutoDraw] PlayerUnits=0` default: a human player's units are never swapped by the dll, the toggle is their switch (in Alex's build; who asked is not quoted) | stance | LN:155, MSR:51-52 |
| D-S19 | Nov 12-13 | `no_target` vehicles: done by SensorTags 0.3, MeleeStance 0.7.38 `[NoTarget]` dropped | stance | DH:105, DH:106 |
| D-S20 | Nov 15 | A player's attack order is honoured and redirected into the charge (no distance limit); `PlayerMaxDistance` 1000 (50 m) for targets of the unit's own choosing | stance | DH:121, DH:122 |
| D-S21 | Nov 20 | "unless the unit has the melee_infantry tag it doesn't cycle their weapons to melee; in melee stance it accepts melee" (`AutoMeleeTag`). Breed coverage still OPEN | stance | DH:179 |
| D-C1 | Nov 7 / 9 | Human fight-distance multiplier to 1.0, then removed: "remove the human stuff from meleechase so it's only fauna" | chase | DH:51, LN:143-145 |
| D-C2 | Nov 9 | Never the word `melee` on a `pattern gun` (vehicle-brain) weapon set; `veh_melee` instead | chase / data | DH:92, KB:27 |
| D-C3 | Nov 19 | Broken gun arm -> melee: first declined as a dll change (DR:223), then "MeleeChase to read tags" (DR:238): entity tags `ai_prioritize_melee` / `_ranged` override the def props | chase | DR:223, DR:238, DH:174 |
| D-C4 | Nov 19 | Ranged priority no longer sets the approach range (engine value kept; counts only weapons that can attack the target); the interim `ranged_fallback` is gone | chase | DH:171, DH:174 |
| D-C5 | Nov 19 | Dread defs: one priority prop each (10 ranged, 2 melee: inferno, ironclad); `dread_limbs` adds / removes the tag | data | DR:235, DR:238; scan |
| D-P1 | Nov 19 | The volume choice is PenFilter's job: `{volume "body"}` removed from vehicle-class rows (kept on grenade / melta rows) | pen / data | DR:236, DR:237, DR:239 |
| D-P2 | Nov 19 | Melee rows at the bottom of the six vehicle classes (PenFilter rejections fall through to them) | data | DR:224 |
| D-P3 | Nov 19 | Heavy bolter / zenite rows cut from medium / heavy / superheavy tank lists | data | DR:218, DR:221 |
| D-P4 | Nov 20 | Aim priority request; default 6 groups: body ; turret,mantlet ; aux_turret1-9 ; arms ; legs ; tracks | pen | PFR:14-21, DH:176 |
| D-P5 | Nov 20 | Vehicle-melee rows (`heavy_veh_melee_target`, `medium_veh_melee_target`) just above the first plain `flame` row | data | DR:247 |
| D-P6 | Nov 20 | HE gating applied: trucks ungated, armoredcar t2+, light / medium t3+, heavy t4+, superheavy t5+, dreads never; `ammo rocketlauncher` rows need `pierce` | data | HE:5 |
| D-F1 | Nov 20 | Flanking (not in 0.1.0, PL:82-84): bolts never flank; superheavies never flank; conquest flag defenders get `ai_no_flank` from Lua; test the 25 m floor / 60 m leash first; end state all AI vehicles; the "flankable" gate goes into the FIRST flanking build | pen + chase | FP:195-202 |
| D-M1 | Nov 20 | Combine the three into one dll | all | PL:3 |

---

## 3. History: the three against each other / other dlls, and the rule each leaves

| # | Symptom (who saw it) | Cause | Fix | Rule for gem_melee |
|---|---|---|---|---|
| H1 fight-distance multiplier | Stance marines "stuck just looking at the guy in melee range" (Alex, Nov 7) | MeleeChase 0.4.0 tripled the brain const `fight/distance` (5 m -> 15 m) while melee was held; a blow only starts inside BlowReach 45 units, so units entered the hand fight at up to 300 units and waited | `fight_distance_mult=1.0`, then removed in MC 0.5.0 | Only one module touches hand-fight distances (BlowReach table); keep `fight/distance` (mod const, 100 units) >= blow reach coherence; the chase module stays human-free (`ENT_FLAGS & 4` skip, mc:858, mc:1028). DH:51, LN:35, LN:144-145 |
| H2 holster / stop-start | Ran-stopped-ran, stood staring ~5 s, "still stop-start", "even worse" (Nov 7) | Every `AddOrder` restarts the running order (stop, re-path); orders every 1.5 s flipping between enemies; stall test per tick; the dll's take-item replaced the attack order; MeleeChase was suspected and cleared (holster once per unit without it) | One order per enemy (`SwitchRatio`, stall sampled once a second on the unit's own position), `DrawCooldownMs` 3000, re-aim thresholds | Never re-issue an order equal to the running one; all re-orders gated on the sim clock; never treat the enemy closing in as the unit's progress. DH:44-47, DH:55, DH:67 |
| H3 attack order fires the held gun | Stance marines jetpacked in, then stood firing flamers and bolt pistols (video, Nov 6) | The double-click eAttack fires whatever is HELD and bypasses MatchAmmo's stance filter | Only units with melee in hand get eAttack / eFight; others get the draw / run first | An attack-type order is given only to a unit with the melee weapon in hand. DH:37, DH:39 |
| H4 attack order does not approach | Units stood with swords out while shot at (Nov 7) | eAttack on a target out of reach of any usable weapon does not move the unit (guns refused, sword 3.3 m) | Run phase: eMovementChange(RunMode 2) + eMoveTask beyond CloseDistance, eFight inside (eAdvance = slow walk, forces mode 1) | The far charge must be a move order. DH:47-49 |
| H5 stare in melee range / run-up-and-stare | (a) BlowReach 50 crossed more bodies, blows refused by the pre-blow ray; (b) attack order issued at 75-124 units stalled just outside reach (Nov 6) | as stated | Rolled back to 0.7.1 + BlowReach 40, later 45 | BlowReach changes need a crowd test; the ray rule (only first hit counts) stays vanilla while `[ShieldMelee]` is off. DH:34, DH:35 |
| H6 stop-and-stare after draw | Units stood 7-17 s after `IN HAND` (Nov 7) | Draw sat under the charge cooldown + a distance progress test | Re-order the moment the weapon is in hand | The draw -> order hand-over is immediate. DH:55 |
| H7 vehicles: inside the hull / around the Russ / far away watching | "trying to move inside them", "ran around the russ a lot", "stood really far from the tank watching it" (Nov 7-8) | Engine range = unit -> body volume CENTRE; arming lapsed on every shove; run goal unreachable (pather stops ~240 units out) beyond CloseDistance 200; infantry-first counted all infantry within 100 m; `InfantryMs` 3000 | Range shift when pressed (ADJ_AHEAD 10 units, AdjacentMax 120), armed for the whole attack order; `[Vehicles] CloseDistance` 400 hand-over; InfantryRange 400; InfantryMs 500; an attack order on a vehicle is never re-issued until the engine ends it | Keep the vehicle hand-over and the hull logic; see OB-14 (rows lost `{volume "body"}` since Nov 19). DH:56-62, DH:64 |
| H8 dread PenFilter standstill | Ranged-priority dreads with a broken main gun stood still; non-AT dreads stood off shooting tracks; "PenFilter standstill" (Nov 19) | MC `apply_priority` held a ranged-priority unit at the reach of its longest non-melee weapon, broken ones included, whether or not it could attack the target | 0.5.4: ranged priority leaves the brain approach range (brain+0x3D4) to the engine; tag priority from `dread_limbs` | Ranged priority must never pin a unit at a range none of its usable weapons reaches. PenFilter rejections reach MeleeChase only through the engine's approach range (inferred: the vehicle evaluation's weapon pick goes through MatchAmmo; the live MSlog `raw:` lines from editor exe+0x82985C carry the dread / Russ pair at the same sim ms as PFlog:16-19). DH:171, DH:174, DR:235, mc:812 |
| H9 dread flamed instead of punching | Assault-cannon dread at the Russ point blank, flaming (Alex, Nov 20) | ammo.set first-match: `flame` rows above the vehicle-melee rows; PenFilter exempts melee, so nothing else reorders | Data: vehicle-melee rows above `flame` (D-P5) | The dll never reorders rows; melee ammo / weapons are never filtered by pen (ExemptTags). Open: why MC's hold-ranged-in-melee did not stop the flamer (OB-6). DR:247 |
| H10 CSM twitch loop | Black Legion marines snap between facings every ~0.5 s near a hostile dread (Nov 20) | Bolter marines have no row vs the dread except `grenade at` + the bottom melee rows; engine offers melee, MeleeStance refuses (ranged stance), front order flips ComplexOrder / MovementChange / none every ~290 ms; also on Guga's 1.0.3 launcher, so not the test dlls | 0.7.42 `AutoMeleeTag` (still returns NULL for the same candidate: whether the flip stops is untested) | A refused melee candidate must not leave a unit flipping orders; log `flip:` (PL:68-70). See OB-1. KB:42-44, BR:693-698, live MSlog orders lines (e.g. MSlog 79647-80234 ms, ~290 ms apart) |
| H11 hold-ranged-in-melee | (a) Suspected of the holster with `hold_ranged_in_melee=1` + `weapon_priority=1` (Nov 7, cleared); (b) dreads with both props hold ranged fire inside reach (Nov 19) | (a) not the cause (DH:47); (b) by design for melee-priority units | - | Hold applies only to vehicle-brain units, never to humans and never to ranged-priority units (mc:1028-1032). See OB-5 (5.8 m loop). DH:46, DH:47, DR:235 |
| H12 direct control taking weapons away | "it takes my weapon away randomly in CQC" (Nov 8); after leaving direct control "still acted like I was in it" | `{if user_control}` = player-owned, not direct control; the manager test never matched in the editor; the per-actor property may stay set after exit | 0.7.11 `IsManualControlled`, 0.7.12 manager +0x3B0, 0.7.14 diagnostics only (no fix recorded) | One direct-control answer for all modules (PL:57-59); see OB-10. DH:65, DH:66, DH:170 (0.7.14) |
| H13 dedicated server crash at match end | Server died 1 s after `Exit game mode` (Nov 9, 3v3) | A freed scene does not clear the dll's weak refs; finished-battle units on the poll list | `mission_flush` on clock going back + `readable(v,0xDB0)` per poll unit | CS-1, CS-3. DH:86 |
| H14 server crash on half-removed unit | `IsManualControlled` read [actor+0xDE0] = NULL, 26 min in (Nov 10) | Engine getter assumes a fully built actor | `ismanual_safe` (both pointers non-NULL) | CS-2. DH:94, ms.c:436 |
| H15 player crash in RefDtor | Bot match 32 min in, unlink walked a freed entity's back-ref list (Nov 10) | Entity freed while the dll's ref still pointed at it | `ref_listed`: trust / unlink a ref only while the entity lists it | CS-1. KB:28 |
| H16 conquest second-battle crash | game exe+0x7E6F6B read at 8 (Tbear, Nov 12) | The new-mission memset zeroed a ref the engine still listed | Wipe releases every ref through `ref_free()` first (0.7.37) | CS-1, CS-3. DH:103, ms.c:2250-2270 |
| H17 server crash at stalk tank spawn | ACCESS_VIOLATION under `actor:update_accuracy` (Nov 9) | `{tag "melee"}` on a `pattern gun` set: engine applies the human Melee skill | Data: `veh_melee` + MC 0.5.1 `vehicle_melee_tag` | CS-10. KB:27, DH:92 |
| H18 hosted-client cost | Client tick 0.95-1.5 ms average with 0 hook calls (Nov 10) | Order poll running on a machine that never simulates picks | `ClientGate` | CS-8. DH:97, DH:98 |
| H19 per-pick cost / MP lag | 21.9 us of dll time per pick vs 0.6 us engine; server 6700 picks / sim s (24000 peak) | `known_note` linear searches, flushes per pick, VirtualQuery in `in_manual` | Tick-only flushes, FastPass, kcache, DiagManual=0 | CS-9. DH:98, KB:29 |
| H20 quant tick | Order tests did nothing: one MatchAmmo call in a whole session; the window-message tick ran on the wrong thread | Engine runs no scan / pick for units under an explicit order; the game window thread is not the engine thread | `Scene::eQuantExecutor` vt[1] slot hook on the engine thread; export `gem_quant_listen`; Teleport must not hook that slot | CS-13. DH:170 (0.7.21-0.7.24), LN:108 |
| H21 0.7 / 0.8 editor crashes | Hard crash ordering a melee on a shielded unit; ACCESS_VIOLATION in eBlow::Apply | Mid-function stub on a misaligned stack; hooked engine function re-called from C with 4 of 7 args | Shadow space + `force_align_arg_pointer`; decide-and-jump stubs | CS-5, CS-6. DH:26, DH:29 |
| H22 three new-mission detectors | Same restart seen at different sim points: MS at 145 / 111 / 111 / 150 ms, MC at 16031 / 503 / 502 / 504 ms, PF at 46181 / 16912 / 35936 ms (and PF saw one restart "from 43623 ms" because its last call in the previous battle came 43.6 s in) | MS: back > 5000 ms (ms.c:2272-2276), MC: back or jump > 5000 ms (mc:644-665), PF: any step back, only when a hook runs (pf.c:1044-1054) | Plan: one detector, all flushed together (PL:52-54) | CS-3. MSlog:723/791/856/1134, MClog:78/86/90/91, PFlog:249/435/525 |
| H23 SM scout melee with the launcher | Scout with an IG chainsword cannot aim / swing with the launcher (Nov 4, handed to Guga) | gem_meleechase suspected (0.4.0 had human hooks) | Not traced; MC humans removed in 0.5.0 | Re-check scouts with melee in the acceptance test (AT-3). KB:21 |

Additional rules that came out of the same history:
- R-H1 Orders are never given inside the enemy scan that found the enemy; they are queued and issued from a later tick (DH:21). `gem_order_listen` / `gem_quant_listen` callbacks must not give orders (LN:109, BR:285).
- R-H2 `CancelInventoryOps` (brain event 0x707) drops the unit's orders too; that is what lets the take-item run at once (DH:170 0.7.25). Keep it before every draw (0.7.7 restored "cancel whenever the inventory is busy", DH:61).
- R-H3 Only one dll may redirect the vehicle approach call and own `Scene::eQuantExecutor` vt[1] (FP:56, LN:108).
- R-H4 PenFilter decides only `pierce` ammo vs vehicle targets and never melee ammo / weapons (PFR:69-81, pf.c:85); MeleeStance decides melee vs gun for humans; MeleeChase decides approach / hold for vehicle-brain units (PL:62-64 R1).
- R-H5 When MeleeStance refuses a candidate it returns NULL without calling the engine, so pen's A never runs for it (ms.c:2047-2056); pen's per-call weapon record is bound to the ctx pointer (pf.c:1225), so a refused call does not leak into the next one. Keep both.
- R-H6 MeleeStance's hull shift writes ti+0x28 around the engine call and restores it (ms.c:2058-2061); pen exempts melee, so it never sees the shifted point. Keep that order.

---

## 4. Open bugs and open risks touching the three

| # | What | Evidence | Touches |
|---|---|---|---|
| OB-1 | Hostile-dread melee loop: a unit whose only rows vs a vehicle are refused melee rows flips orders every ~290 ms. 0.7.42 still returns NULL for the same candidate; untested. Fix options (Alex to choose): "cannot attack this target" memory in the dll, or ammo.set changes | KB:42-44, BR:697-698, scan: `dreadnought` class has no `bolt` row (ammo.set:713-760) | stance, pen (bolt / las ammo carry `pierce`; a REJECT also falls through to these rows, BR:589, inferred) |
| OB-2 | Post-fight staring of CSM marines (LookAt / ComplexOrder / Advance flips among friends, no ammo picks); unexplained; lead: wounds + stimpack / bandage AI | KB:43-44 | stance (order poll only logs) |
| OB-3 | `AutoMeleeTag=melee_infantry` covers 6 breed files only (scan: `breed\mp\tg\warhammer\bp_cultist5, bp_storm_melee, dg_cultist5, dg_melee, dg_vet_melee`, `breed\mp\csm\warhammer\bp_cultist5`); assault marines, raptors, orks, cultist6, commissars, officers would never melee outside stance; choppa-only grots / slugga boyz lose their only weapon. Decision open: empty value (= 0.7.41), `melee_auto` second tag, or `melee_infantry` (adds [Zerg]) | DH:179, MSR:11-14, scan | stance |
| OB-4 | Quant tick is one-shot: on the 1.068 game exe (Nov 17) the slot belonged to `.?AVeMainThread@Core@@`, frame hook FAILED, ticks 0; unknown on the 1.069 game and server exe (editor 1.069 OK: MSlog:72) | BR:314 | stance (order poll, flushes) + Teleport (NR-X1) |
| OB-5 | MC 5.8 m oscillation: melee-priority unit alternates "target in melee reach - ranged weapon holds fire" (hold uses <= 1.0 x reach, 2D) and "inside weapon range - chasing" (chase_always > 0.9 x reach); 18 approaches in 11 s, never closer. Probably the same test as DR:247 (inferred from the time) | MClog:27-77 (5.8 m lines 36-77), BR:463, mc:1033 vs chase 0.9 rule | chase |
| OB-6 | Hold-ranged-in-melee did not stop the dread's flamer in that test (it flamed point blank); hold only redirects the vehicle evaluation's SetTarget sites (player-forced targets take another path, MCR:112) | DR:247, MCR:102-112 | chase |
| OB-7 | MC counts broken / script-disabled weapons in reach (`{weapon_work "melee" 0}` on dreads) | BR:462, mech_dreadnought.inc:761-775 | chase |
| OB-8 | MC ranged-priority units still get the approach shrink / re-path (t_chase set whenever a melee weapon exists) | BR:461 | chase |
| OB-9 | `gem_order_listen` hookup lives in `hb_thread`, created only when `[MeleeStance] Log=1`; with Log=0 no order events | ms.c:1897-1907, ms.c:3158 | stance + Teleport |
| OB-10 | Direct control: the manager test never matched in the editor (live log `property 1, manager 0` for every skip, MSlog:154-187, 362, 412-417); the server exe has no manager; the per-actor property may stay set after exit (DH:170 0.7.14, no later fix). PenFilter reads the same two halves inline (pf.c:739-746). Whether the property is replicated / sim state on every machine is inferred | DH:65-66, DH:170, PFR:269-271 | stance, pen |
| OB-11 | "sometimes not taking out the melee weapon": AI `auto-draw: take-item order REFUSED / no item tags` and 6 `REFUSED by AddOrder` | DH:121 | stance |
| OB-12 | PenFilter ALLOW with a face threshold 0 ("needs 0.0"): assault cannon vs Russ at 144.9 m, 40mm AP vs a vehicle, and `lasgun pistol` shooters vs the dread at 7.1 m (infantry plinking would be allowed) | PFlog:16, 128-132, 276, 376; BR:577 | pen |
| OB-13 | PenFilter X weapon stop is a new call place for the engine cease-fire; never fired live (0 stops); an eAttack re-fires its own aim record after a stop | PFlog:620, PFR:251-257 | pen (+ chase hold uses the same engine clear, PL:35-37) |
| OB-14 | Vehicle-class melee rows lost `{volume "body"}` (Nov 19). MeleeStance's hull logic (0.7.3-0.7.10) was tuned when the target point was the body volume centre; a row without a volume may land on any non-`no_target` volume (CLAUDE.md "AI aim volume"), and pen's P does not cut lists for exempt melee ammo. Effect on stance-vs-tank untested | DH:57, MSR:183-191, DR:237, ammo.set vehicle rows (e.g. 547-548) | stance, pen |
| OB-15 | Gun-only tank with every row rejected: keeps or drops the target, holds fire (flanking RE item 6, HE Q8) | FP:115-119, FP:200-202, HE:147-153 | pen, chase |
| OB-16 | Pen keeps ALLOW on a target whose pierceable volumes are all hidden; the unit may keep it and not fire | PFR:266-268 | pen |
| OB-17 | MeleeChase build has no `-mcpu=baseline -ffp-contract=off -fno-fast-math` (build.bat:21); MeleeStance / PenFilter have them | build.bat files | chase (MP determinism) |
| OB-18 | MeleeStance tick throttle uses wall time (TickMs/2) plus "sim clock moved" (BR:634). Only harmless if one machine simulates (dedicated server / local game; DH:97 client 0 picks). Conflicts with the plan's R7 wording "simulation state, sim clock and entity ids only" | BR:634, PL:73-75 | stance (MP) |
| OB-19 | Mixed builds in one match desync: Guga's live 1.0.3 has MS 0.7.41 + MC 0.5.3 and no PenFilter; a gem_melee build differs on every machine that lacks it | BR:454, BR:572, LN:181 | all |
| OB-20 | Server late load: when the launcher's early-bird APC path fails, payload dlls are injected into an already-running server with remote threads (ScriptExt and Teleport first, then payload order) | IndomitusLauncher_mc054 `launcher\main.cpp:1385-1392, 1530-1540` | all (install while the engine runs) |
| OB-21 | `ProbeAnyRange`: live payload ini 0 (Guga's ini, a1d6ed3c `gem_meleestance.ini`:154), source ini / code default 1 (ms.c:260, ms.c:3043); who set 0 is not in the notes | inputs | stance |
| OB-22 | MC `chase_run_mode` calls the chassis SetMovementMode slot that GEM Fauna wraps; whether it survives Fauna's reset is unknown | BR:442, BR:470 | chase |
| OB-23 | MC apply_priority uses 3D distance, the chase / hold rules 2D | BR:464, mc:817-819 | chase |
| OB-24 | `[Downed]` refuses every pick vs a downed human for every shooter, vehicles included (not only infantry) | BR:326, ms.c:2037-2041 | stance |
| OB-25 | 0.7.42 `AutoMeleeTag` gates also the probe / blind draw for AI; the melee_toggle button draw is not gated on purpose | DH:179, MSR:17-22 | stance |
| OB-26 | Stale docs that a port could copy: MeleeStance README settings table (`melee_vs_human / melee_vs_all`, CooldownMs 4000, MoveRefreshUnits 80); MeleeChase README / ini still say ranged priority "holds at its ranged reach" and tell to put `{tag "melee"}` on fauna guns | BR:318, BR:458-460, MSR:303-322, MCR:55-63, MCR:69-71 | docs |
| OB-27 | Teleport finds `gem_quant_listen` only in a module named `gem_meleestance.dll` (teleport.c:504) | inputs\teleport_0.2.13 | stance export |
| OB-28 | `hull_contact` (ram-volume touch tag) never fired in the 0.7.5 test; the stall sample does the work | DH:59 | stance |

---

## 5. Mod-side interface (grepped Nov 20 2026)

### 5.1 Entity / breed tags the dlls read

| Tag | dll (ini key) | Set / used in the mod (grep) | Status |
|---|---|---|---|
| `melee_stance` | stance `[MeleeStance] Tag` | set / removed in `set\interaction_entity\40k_human\40k_human.inc:1121-1137` (`{on "melee_toggle"}`, after `{delay 0.1}`); read for the icon in `40k_skin\skin.inc:63` | OK. The 0.1 s delay is load-bearing: the draw decides "being switched on" from the still-untagged unit (DH:40, DH:34) |
| `melee_always` | stance `AlwaysTag` | none | unused (0 hits) |
| `melee_infantry` | stance `AutoMeleeTag`, `[Zerg] Tag` | 6 breeds (OB-3) | coverage decision open |
| `melee_auto` | proposed 2nd AutoMeleeTag (ini comment) | none | unused |
| `has_melee_weapon` | stance `[Charge] Tag`, in-hand tests | added 21x in `40k_human\40k_skin\skin_weapon.inc:584-892`, removed at :926; read by FSM files and `40k_human.inc:26` | OK |
| `hull_contact` | stance `[Vehicles] ContactTag` | `40k_human.inc:25-28` (`{on contact}` `{if volume "ram"}`, 1.5 s) | OK, never seen firing (OB-28) |
| `start_personage` | stance `[Downed] Tag` | `40k_human.inc:1077-1111`, vanilla-copy `set\interaction_entity\human.inc:110,389,740,1222-1241`; ability incs | OK |
| `tp_jetpack` | stance `SkipTags` | `40k_human.inc:722` add, `:724` 6 s failsafe, `:806` remove (Teleport flow) | OK |
| `hidden` | stance `SkipTags`, `IgnoreTargetTags` | conquest / frontlines mission scripts (`map\multi\...`, 67 files) | OK |
| `melee_disabled` | stance `SkipTags`, `IgnoreTargetTags` | none | unused |
| `disable_melee` | stance `SkipTags` only | only `map\test\40k_conq_triggers.mi:64,141,269`, `map\test\test_conq.mi` (old `melee_veh_*` test triggers) | test maps only |
| `valkyrie` | stance `SkipTags`, `IgnoreTargetTags` | `set\interaction_entity\40k_vehicle\40k_airborne\airborne_main.inc:63` | OK |
| `sensor_off` | stance `SkipTags`, `IgnoreTargetTags` | `40k_spawners\astartes.inc, conquest_callins.inc, generic_faction.inc, impguard.inc, traitorguard.inc`, `40k_airborne\astartes_airborne.inc` (also SensorTags) | OK |
| `forcefield` | stance `[ShieldMelee] SeeThroughTag` (off) | `set\interaction_entity\40k_forcefield.inc:8` (shield `{on spawn}`) | OK, feature off |
| `ai_prioritize_melee` (tag) | chase `prop_prioritize_melee` (0.5.4 tags) | only `set\interaction_entity\40k_vehicle\40k_mech\mech_dreadnought.inc:780-783` (`dread_limbs`) | OK, untested |
| `ai_prioritize_ranged` (tag) | chase | none | prop only |
| `no_target` (entity tag) | none of the three (SensorTags); 0.7.38 rule dropped | spawners | not a melee-dll tag |
| `ai_flank`, `ai_no_flank` | planned flanking | none | not in 0.1.0 |

### 5.2 Ammo / weapon words

| Word | dll (key) | Where in the mod | Status |
|---|---|---|---|
| `melee_light`, `melee_heavy` | stance `MeleeAmmoTags`, `VehicleMeleeTags`; pen `ExemptTags` | `set\stuff\melee\melee_ammo_light.ammo:2` (`melee_light pierce`), `melee_ammo_heavy.ammo:2`; rows in every ammo.set class | OK |
| `medium_inf_melee_target`, `medium_veh_melee_target`, `heavy_veh_melee_target` | stance `MeleeAmmoTags` | `set\stuff\shell\*_melee.ap.ammo:2` (each also `melee`, and `pierce` from `ap.pattern`); used by vehicle-brain units (dreads, stalk tanks) | stance acts on humans only, so these matter only if a human carried them; pen exempts them through `melee` |
| `melee` (stuff tag / name word) | stance `[AutoDraw] Item`, `melee_toggle.set {item}`; chase `melee_tag`; pen `ExemptTags` | `{tag ... melee}` on IG/TG melee sets in `set\stuff\melee\`, all 43 `_spacemarine\melee\` sets, 4 `_ork\melee\` sets, the 3 `*_melee.ap.ammo`; 4 knives in `stuff\melee\` (`ig_knife`, `knife_krieg`, `knife_tanith`, `tg_cultist_knife`) have no `melee` tag and rely on the folder word (inferred, DH:44 says the 0xEC8 Tags carry folder words). No `stuff\gun` set carries `melee` (rule D-C2 holds) | OK |
| `veh_melee` | chase `vehicle_melee_tag` | 8 sets in `set\stuff\gun\`: `melee_bloodcrusher`, `melee_bloodletter`, `melee_dreadnought_claw / _fist / _stomp`, `melee_ogryn` (`strong veh_melee`), `melee_stalktank`, `melee_stalktank_heavy` | OK (Ogryns now chased; 0.5.x server log said "not chased", DH:97) |
| `pierce` | pen `PierceTag` | 95 of 214 ammo files + 9 patterns (BR:534) | OK |
| KeepTags `mgun, hmg_shell, stubber, lascannon, plasmacannon, sm_weapon_plasmacannon, sm_weapon_heavybolter, rocketlauncher, heavy_flamethrower, grenadelauncher, at_rifle, ogryn_weapon` | stance `[ReloadCancel] KeepTags` | every word exists on some `set\stuff` file (scan); the list is hand-made (no melta, HKM, plasma gun words) | OK; since 0.7.24 the fight-range path ignores KeepTags (DH:170 0.7.24) |

### 5.3 Def props
| Prop | dll | Where |
|---|---|---|
| `ai_prioritize_melee` | chase | `bp_stalk_tank.def:4`, `sm_dreadnought_inferno.def:6`, `sm_dreadnought_ironclad_hurricane.def:6`, `t_bloodletter\Patterns.inc:11`, `t_bloodcrusher\Patterns.inc:11`, `t_ogryn\Patterns.inc:11,41,99,136` |
| `ai_prioritize_ranged` | chase | `bp_stalk_tank_scorpian.def:4`, 10 `sm_dreadnought_*.def:6/10`, `t_ogryn\Patterns.inc:70` |

### 5.4 Buttons, events, script handlers, consts
- `interface\action\melee_toggle.set`: `{item "melee"}` + `{item_in_box}` (CanApply = "carries melee", 0.7.19; take-item tags at action+0xC8), `{button "melee_toggle"}` (= `DrawButton`, read at action+0x60), `{Key "actions/melee_toggle"}` (launcher hotkey SHIFT+C, LN:155), `{apply_to_all}` (multi-select), `{instant {if props "human" {effect "melee_toggle"}}}`. Slot `set\panel_control\right_attack.set:77`.
- Script event `melee_toggle` handled in `40k_human.inc:1121`; stance icon views `melee_stance_icon` / `ranged_stance_icon` (`40k_skin\skin.inc:57-67`, hidden in `human.inc:434-435`). The dlls fire no script events and listen to none (BR:294, BR:440, BR:541).
- Brain const `fight/distance 5` (= 100 units) in `properties\40k_infantry\40k_human\40k_human.inc:204`: `[AutoDraw] Distance=100` is tuned to it (MSR:139).
- Sword range `{range 3.3 3.3}` (66 units) in `set\stuff\melee\sword.pattern:25`; Range() of melee AMMO is 0 (range sits on the weapon set, DH:59): `[Charge] MinDistance 60`, `ADJ_AHEAD 10` are tuned to these.
- BlowReach overwrites the global vanilla `set\blow` start table (17/29/35 -> 45) for every paired fight (MSR:134-136).
- `40k_human.inc` `{on contact}` ram branch requires `has_melee_weapon` (40k_human.inc:25-28).
- Dread script: `dread_limbs` adds / removes `ai_prioritize_melee` (mech_dreadnought.inc:780-783); `{weapon_work "melee" 0}` (761-775) is invisible to MC (OB-7).
- Mission triggers: no `melee_inf_atk` / `melee_veh_*` block is left in `map\multi` (grep); only `map\test\40k_conq_triggers.mi` and `test_conq.mi` keep `melee_veh_*` blocks with `disable_melee`. Without the dll there is no infantry melee charge at all (DH:83).

### 5.5 ammo.set rows the dlls rely on (`set\target\ammo.set`, file of Nov 20 01:40)
- Human classes `human in_bush` (21), `human in_cover` (75), `human` (129): `melee_heavy`, `melee_light` rows (58-59, 112-113, 160-161) below the guns, no `{ready_for_shot}` on any melee row today (so `DrawFromBelt` has nothing to act on; inferred from the row list).
- `fauna` (182), `car` (221), cannon classes (263, 307, 352, 397), `howitzer` (443): melee rows, `{volume "body"}` kept on cannon / car rows.
- Vehicle classes `armoredcar_l` (503), `armoredcar` (551), `tank_light` (605), `tank_medium` (660), `dreadnought` (713), `tank_heavy` (761), `tank_superheavy` (810): `heavy_veh_melee_target` / `medium_veh_melee_target` just above `flame` (D-P5), infantry `melee_heavy` / `melee_light` / `medium_inf_melee_target` at the bottom (D-P2), no `{volume "body"}` on them (D-P1). Stance vehicle melee (`VehicleMeleeTags`) and pen's fall-through both need these bottom rows. `dreadnought` has no `bolt` / `heavy_bolt` row (OB-1).
- Target class `dreadnought` (40k_dreadnought.ext) is new since Nov 20 (HE:5).

### 5.6 Volume tags (pen `AimPriority`)
All default AimPriority words exist in the mod's `properties` / `entity` ext / def / inc files (file counts: bodyback 31, mantlet 104, aux_turret1 69, aux_turret9 6, arm_left 13, gun_left 13, hkm_left 2, leg_left 36, frontleftleg 2, backrightleg 2, trackleft 270, wheel 103, cabin 2). Volume tag `no_target` (vehicle.ext defines, dread ext, Taurox, Crassus, Sentinels, Tauros...) keeps a volume out of the picker and out of pen's verdict (BR:537).

### 5.7 Things that name the dlls or parse their logs
| Where | What | Effect of the merge |
|---|---|---|
| Mod comments (no code effect): `mech_dreadnought.inc:780`, `40k_human.inc:25,1121`, `40k_forcefield.inc:8`, `set\stuff\gun\melee_*` line 2 (8 sets) | name "GEM MeleeChase 0.5.x" / "MeleeStance dll" | stale wording only; plan: no mod change in 0.1.0 (PL:82-84) |
| No mod file names a dll file (`gem_*.dll`) or a dll log line (grep of set / properties / map / script / interface) | - | - |
| `_tools\server_tickrate\hb.py`, `simspeed.py` | regex on MeleeStance `hb: [N ms] hook calls N ... ticks N ... poll list N ... tick cost: N us total, N us average, N us worst`, file `gem_meleestance_server.log` | keep the `hb:` text unchanged after any prefix (re.search tolerates a prefix) or update the tools; log file name changes |
| `_notes\bot_lua_qc_plan.md:112` (T6 botlog.py) | plans to parse MeleeStance `hb:` lines | same |
| Launcher worktree `launcher\res\features.txt:17-38` | cards `[gem_meleestance]`, `[gem_meleechase]` (none for penfilter); a dll without a card is listed by file name | a `[gem_melee]` card is Guga's repo |
| Launcher `main.cpp:1530` `kServerFirst` = scriptext, teleport | none of the three | gem_melee loads in payload order (after itempen) |
| GEM Teleport 0.2.13 `teleport.c:504-509` | `GetModuleHandleA("gem_meleestance.dll")` + `gem_quant_listen` | needs PL:76-79 (0.2.14 or forwarder) |
| MeleeStance `ms.c:1905` | `gem_teleport.dll!gem_order_listen` | unchanged import |

### 5.8 Mismatches (ini lists vs what the mod uses)
1. `AlwaysTag=melee_always`, `melee_disabled` (SkipTags / IgnoreTargetTags) and the README example `melee_auto`: no setter anywhere in the mod.
2. `disable_melee` (SkipTags): only in two test-map files; not in IgnoreTargetTags (asymmetric with `melee_disabled`).
3. `AutoMeleeTag=melee_infantry`: 6 breed files vs ~280 melee carriers (MSR:11-14); the a1d6ed3c payload still runs 0.7.41 (no AutoMeleeTag key).
4. `ProbeAnyRange`: payload 0 vs source / default 1 (OB-21).
5. README default `MeleeAmmoTags = melee_vs_human, melee_vs_all` (MSR:307): neither word exists in the mod; the code default (ms.c:58, ms.c:2980) and ini are right.
6. MeleeChase README tells to tag fauna guns `melee` (MCR:55-63): contradicts D-C2; the mod uses `veh_melee`.
7. `[Vehicles] VehicleMeleeTags=melee_light,melee_heavy` excludes the `*_melee_target` words that `MeleeAmmoTags` includes; consistent with "stance humans use hand melee ammo vs vehicles", but the two lists differ on purpose only by inference.
8. KeepTags is a hand list; heavy weapons without one of the words get their reload cancelled on a draw (inferred from the list; no note).
9. Vehicle-class melee rows have no `{volume "body"}` any more while the stance hull logic was tuned with it (OB-14).

---

## 6. Crash-safety checklist for the code review

- [ ] CS-1 Weak refs: every engine ref the dll holds is released through the engine RefDtor only while the entity still lists it (`ref_listed`, back-list offset read from RefDtor, +0x198 on 1.067-1.069); a ref no longer listed is zeroed without unlink; if the offset is not found, the log says the stale-ref check is OFF (KB:28, DH:103, ms.c:796-826, ms.c:2250-2257).
- [ ] CS-2 Half-built / half-removed units: before calling an engine getter found by signature on a unit taken from a list, check the sub-objects it dereferences (`IsManualControlled`: [ent+0xDE0] and [ent+0xDD8] non-NULL, ms.c:436); poll units get a `readable()` guard, cached per sim-clock value (DH:86, DH:94).
- [ ] CS-3 New mission / new battle: one detector, every module's tables (stance g_seen / g_unit / g_pend / g_last / g_dpend / g_known / g_kcache / g_mmc / g_want / g_mine / g_hostile; chase g_ch / g_tr; pen c_blk / c_hold / c_unr / memo tables / log table) flushed at the same sim point; refs released first (CS-1); covers clock back (all three) and the forward jump of MC (mc:644-665); editor sim start / stop restarts the clock too (DH:82). Detector must run even when no pick happens (H22).
- [ ] CS-4 Entity pointers kept across ticks are compared, never dereferenced, unless resolved through a live ref (MC record entity, mc comments at BR:428).
- [ ] CS-5 Mid-function stubs keep the host's stack alignment (entry: rsp = 8 mod 16; body: usually 0) and give C 0x20 shadow space; `force_align_arg_pointer` on C targets (DH:26).
- [ ] CS-6 Never re-call a hooked engine function from C unless its full argument list is known; use decide-and-jump stubs (eBlow::Apply reads 7 args, DH:29).
- [ ] CS-7 No orders from inside the enemy scan or from listener callbacks; queue and issue from the tick (DH:21, LN:109).
- [ ] CS-8 Clients of a hosted game never get a weapon pick: tick / poll stay idle until this process has seen one (ClientGate, DH:97-98); the server exe has no eManualControl manager ("not found (server exe: expected)" must not turn anything else off, DH:66).
- [ ] CS-9 Hot path (per pick, per shot, per unit per quant): no VirtualQuery (1.6 us) / ReadProcessMemory (0.32 us) / string tag scans (1.1 us per 40 tags); interned tag ids (0.011 us); answers cached per sim-clock value; DiagManual off by default (memory note dll-hot-path-probe-costs, DH:98). Baseline to beat: live editor `hb:` 0.53-2.3 us dll time per call, engine MatchAmmo share ~1.6 us with pen's A inside (MSlog hb lines at 285427 ms and the last line).
- [ ] CS-10 The dll never encourages `melee` on a `pattern gun` set: vehicle melee stays `veh_melee` (KB:27).
- [ ] CS-11 Stance units attack vehicles through script contactors whose damage needs GEM ContactDamage; a payload without it crashes on every vehicle melee hit (CLAUDE.md, BR:714). gem_melee must not be shipped without gem_contactdamage.
- [ ] CS-12 Patch registry: refuse to patch bytes that are not the expected original (pen's rule, PFR:279-280); MC's vt_hook / redirect_call chain on whatever is there (BR:445): in one dll every write is claimed once (PL:54-55); one owner of the approach call and of `eQuantExecutor` vt[1] (R-H3).
- [ ] CS-13 The quant-hook install can fail (OB-4): the MatchAmmo-path fallback for flushes / poll must still work, and Teleport's `gem_quant_listen` registration must not crash when no tick ever comes (DH:98 kept the old path; LN:105 lag only).
- [ ] CS-14 Full tables: no silent eviction of live per-unit state that makes units re-ordered every scan (0.7.39: NLAST 1024, NMINE 256, NPEND 256; DH:120); MC 0.5.3 velocity slots only taken over when older than 3 s (MCR:163-171).
- [ ] CS-15 Sim decisions use sim state, sim clock and entity ids only: no pointer-keyed sim state (MC 0.5.3 lesson), no RNG, no wall clock (OB-18); same compile flags for all code (OB-17).
- [ ] CS-16 Threads: the tick acts only on the engine thread the dll saw the engine use; the multi-select / toggle draw runs on the UI thread and calls AddOrder there (BR:282, BR:303); per-pick state is thread-local and bound to its ctx (pf.c:1225).
- [ ] CS-17 Install is safe in an already-running process (launcher server late-load path OB-20; `inject.bat` tests): write the jump last, VirtualProtect + FlushInstructionCache, lazy pieces (tag ids, tick hook) built on the engine thread (MC builds tag ids lazily, BR:439).
- [ ] CS-18 Logging off (`Log=0`) must not switch interop off (OB-9); log caps reset per mission (DH:67); logs readable with shared access while the game runs (DH:170 0.7.27).
- [ ] CS-19 One vectored exception logger, first-chance only, always CONTINUE_SEARCH (MC crashlog, BR:350); the IAT exit trace hooks nothing in practice (BR:351).
- [ ] CS-20 Hull shift restores ti+0x28 on every path after the engine call (ms.c:2058-2061); pen's P keeps the picker's "previous volume" index consistent with the cut list (PFR:90).
- [ ] CS-21 Pen's X calls the engine weapon stop from inside the picker (new place); `ExhaustStop=0` stays as the off switch; never stop a melee weapon; the chase hold never clears a weapon pen is holding on a pierceable volume of another target (PL:66-67, R3).
- [ ] CS-22 Guard: if gem_meleestance / gem_meleechase / gem_penfilter is also loaded, the matching module stays off (PL:80-81); the launcher kills the game when a payload dll is missing from the module list after 5 s (LN:8), so a guard must not unload itself.

---

## 7. In-game acceptance checklist for Alex

Setup: test launcher = today's payload with the three dlls swapped for `gem_melee.dll` + `gem_melee.ini` (same values as
`MeleeStance 0.7.42` src ini with the AutoMeleeTag choice Alex makes for OB-3, MeleeChase 0.5.4 ini, PenFilter 0.1.1 ini).
Logs in `%LocalAppData%\Indomitus Launcher\payload\<fp>\gem_melee_editor.log` (game: `gem_melee.log`, server:
`server\gem_melee_server.log`). Expected lines are the old texts with a module prefix.

| # | Steps | Expected |
|---|---|---|
| AT-0 Start | Open the editor through the launcher | One banner `GEM Melee 0.1.0`; `[stance] ... installed: ammo match exe+0x7AADA0`, `charge ON`, `multi-unit instant buttons ON ... 9/9 vtables`, `auto-draw ON`, `vehicles ON`, `reload cancel ON`, `direct control ... IsManualControlled exe+0x67E5C0`, `downed targets ignored`, `blow reach ON: near/medium/far 17/29/35 -> 45.0/45.0/45.0`, `no-melee rule ON/OFF`; `[chase] humans not touched`, `vehicle melee chase ON`, `weapon priority entity tags ON (entity +0x4E8 ...)`; `[pen] row filter (hook A) installed`, `aim check (hook B)`, `aim priority (hook P)`, `exhausted-pick hook (X)`; no line saying OFF / FAILED / not found except the expected server-only ones. Teleport log: `order listeners: callback ... registered`, and (after the first sim tick) `aim: per-quant tick ... registered`. After pressing play: `[stance] tick: .?AVeQuantExecutor@Scene@@ ... hooked` |
| AT-1 Toggle + charge | Select 5 assault marines, press the melee toggle, double-click an enemy squad 40 m off | Popup On on all 5, melee icon; `draw:` take-item lines; `charge: ... run` then `melee` inside 10 m; they fight. Toggle again: popup Off, ranged icon for 2 s |
| AT-2 Fight range (AI) | AI tactical marines (knife on the belt) meet AI guardsmen at 5 m | Knife out within ~1 s, fight order; gun back once nobody is within 7.5 m (`ranged:` lines) - only if Alex's AutoMeleeTag choice lets these breeds melee |
| AT-3 Scouts | SM scout with chainsword / combat knife in stance | Swings (KB:21 regression check) |
| AT-4 melee_infantry bots | `dg_melee` squad (AI) 60 m from enemy infantry | Runs in and fights ([Zerg]) |
| AT-5 Twitch | CSM bolter squad vs a hostile SM dread on test.mi, 2 min | No facing snaps; no `[core] flip:` block; `no-melee:` lines instead of order flips every ~0.3 s |
| AT-6 Stance vs tank | Stance squad on a Leman Russ | `attack-vehicle`, `pressed against`, swings land, Russ dies; no running round the hull (watch OB-14) |
| AT-7 Dread vs Russ | AI assault-cannon dread vs AI Russ | `[pen] A ... REJECT` for the AC drum at the front; `[chase] melee-priority unit ... approaches`; the dread punches (not flames); no 5.8 m loop (OB-5) |
| AT-8 Russ vs dread | AI Russ (AP + HE) vs a dread | AP / HE changes at most once per 8 s (`A` lines), reloads do not repeat every second, `P ... aim group` lines |
| AT-9 Broken arm | Break a ranged dread's right arm (gun_right) | `[chase] unit ...: entity tag "ai_prioritize_melee" -> melee priority`; it charges; repair -> back to ranged |
| AT-10 Direct control | Take a stance marine in direct control mid-fight, then a Russ under pen | No weapon swap / charge for the driven unit (`manual:` lines), weapon never taken away; `direct control skips` counts up in `[pen] stats`; after release the unit behaves normally again (OB-10) |
| AT-11 Restart | Stop and restart the scene twice | One `[core] new mission` line per restart, all three modules flushed at the same sim ms; first pen verdicts not `held since` an old time |
| AT-12 Cost | 10 min bot battle | `hb:` dll time per call not above the 0.7.41 + PF numbers (MSlog: ~0.5-2.3 us dll, ~1.6 us engine incl. pen) |
| AT-13 Hosted MP | Host a 2-player game (both on the same test launcher) with bots, melee squads, a dread and tanks, 15+ min, end the match, play a second battle | Server log `server\gem_melee_server.log` has the install lines; client log shows the tick gated (no picks); no client or server crash, including at match end and in the second battle (`new mission ... refs released` on the server, exit code 0); jetpack jumps show on the client; the client player's stance toggle and charges work; no desync messages in game.log |

---

## Appendix: editor 1.069 addresses from the live logs (payload a1d6ed3c, ground truth for T3)
- Stance (MSlog:1-15): MatchAmmo exe+0x7AADA0; scan exe+0x7EF410; MakeAttack exe+0x77F5D0; AddOrder exe+0x67E750; ref vt exe+0xE9D4F8; range exe+0x66EE20; clock exe+0x13DF988; stamp exe+0x13DF928; ExecuteInstant exe+0xAA59F0 (9/9 vtables, apply_to_all +0x498, GetActor exe+0xAB8CA0, expand exe+0xAB8640); TagContainer ctor exe+0x3E4E60, FromString exe+0x3E60D0; eMoveTask exe+0x7888F0; eFight exe+0x77CC00; eMovementChange exe+0x788C00; busy exe+0x659050; cancel exe+0xAAD6F0; GetCurrentWeapon exe+0x695600; IsManualControlled exe+0x67E5C0; manager exe+0x142C678 (+0x3B0); blow table exe+0xF71368, GetBlowDistance exe+0x7F8910; quant run function exe+0x413560, slot exe+0xF381D0 (MSlog:72); linked +0x1C0 bit 28 (95 sites); ref list +0x198.
- Chase (MClog:1-26): weapons item tags +0x11E0; eOutdoorsBattle slots 3/4 exe+0x7E1A10 / exe+0x7E1290; const getter exe+0x799C10; eAttack reader exe+0x782C12; current weapon exe+0x695600; eVS_Root tick exe+0x82F700; approach call exe+0x82FE14 -> exe+0x788A50; eMoveTask Update exe+0x78BC80; clock exe+0x13DF988; approach range brain+0x3D4; def props +0x548 -> +0x330; weapon range exe+0x699EA0; entity tags +0x4E8 (no_auto_repair test exe+0x82F3D4, HasTag exe+0x10A3B0); chassis SetMovementMode vt +0x168; target evaluation exe+0x828240 (from exe+0x827927); SetTarget exe+0x6998E0 (4 sites); clear exe+0x699CC0.
- Pen (PFlog:1-15): MatchAmmo exe+0x7AADA0 (A1 exe+0x7AB02A, A2 exe+0x7AAFC3); builder exe+0x7AACC0; scan returns exe+0x7EF741 / exe+0x7EF8AC; armour block exe+0x675C40, evaluate exe+0x675CD0; solver call exe+0x7A9D0D -> exe+0x6AEB80; try-loop exe+0x7A9BFA (candidates exe+0x140ED60); clear call exe+0x7A9D49 -> exe+0x6CE920; aim volume exe+0x6CDE20; weapon stop exe+0x699CC0; direct control exe+0x67E5C0; sim clock exe+0x13DF988.
- Shared by more than one module: MatchAmmo exe+0x7AADA0 (stance prologue + pen A1 / A2 inside); sim clock exe+0x13DF988 (all three); GetCurrentWeapon exe+0x695600 (stance, chase); engine clear / weapon stop exe+0x699CC0 (chase hold, pen X); IsManualControlled exe+0x67E5C0 (stance call, pen inline read); AddOrder exe+0x67E750 (stance; GEM Fauna's squad pace also calls it, BR:605).
