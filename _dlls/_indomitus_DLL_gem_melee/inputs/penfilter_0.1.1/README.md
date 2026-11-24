# GEM PenFilter 0.1.1

The AI stops firing `pierce` ammo at vehicle armour it cannot penetrate, and when it can penetrate
something it aims at the most wanted part of what it can penetrate (body, then turret, then arms, then
legs, then tracks). When nothing a unit carries can penetrate, the engine falls through
`set\target\ammo.set` to the next rows, so the melee rows at the bottom of every vehicle class get picked.

Call to Arms: Gates of Hell 1.068 / 1.069 (game, editor, dedicated server; exes of Nov 17 2026). Built Nov 20 2026.
**Status: not run in game yet.** The finders, the patch writes, the asm stubs and the new logic pass the
offline self-test on all three exes (`out\selftest.txt`); nothing below has been seen working in a battle.

## What changed in 0.1.1

Alex's request + the fixes from the first in-game test (Russ AP / HE flip, picks running out):

1. **Aim priority (new hook P).** Before the volume picker starts its tries, its candidate list is cut down
   to the volumes the loaded ammo can pierce (same face test as hook A) of the highest-priority non-empty
   `AimPriority` group. The engine's own random pick then lands in that group. Volumes the solver could not
   reach on an earlier cut list (no solution, elevation limit, line of fire blocked) are skipped for
   `UnreachableMs`, so the next pick moves on to the next pierceable volume or group (a hull-down tank:
   body hidden, turret visible -> turret). No reachable pierceable candidate = the list is left as it is.
2. **No more `BlockStreak` ban.** B now caches as blocked both the volume the round hits first and the
   volume the picker aimed at (a volume hidden behind a plate); A and P skip those. A blocked volume stays
   out until the direction to the shooter in that volume's own frame turns by more than 25 degrees (target,
   turret or shooter moved round), or the shooter comes more than `HoldDistanceM` nearer; `CacheMs` is only a
   safety limit now (60 s). Moving away keeps it (penetration only falls). An ammo is rejected only when no
   candidate volume is left. `BlockStreak` in an old ini is ignored with a log line.
3. **Picks that run out (new hook X).** When a pick ends with no candidate left and B blocked at least one
   of its tries, the engine's clear still runs, then the ammo verdict against that target is worked out
   again without the blocked volumes (REJECT only if none is left) and held. A weapon whose aim slot holds
   that target is stopped with the engine's own cease-fire call (`ExhaustStop`) only when that aim can no
   longer pierce: verdict REJECT, or the volume in the slot is blocked / fails the face test. A weapon firing
   at a volume it can pierce (say the tracks, a lower group) keeps firing.
4. **Ammo-choice hold.** A's verdict per shooter / target / ammo + weapon (ALLOW / REJECT) is kept for
   `VerdictHoldMs` of sim time. A REJECT is dropped early when the shooter comes more than `HoldDistanceM`
   nearer, an ALLOW when it moves that much farther away, and either one when the target moves into or out
   of penetration range. Together with 2 this stops the AP / HE flip and the repeated reload starts: a REJECT
   worked out again from the blocked volumes lasts until the geometry really changes.
5. **Logging.** A verdict line is written only when the verdict of (hook, shooter, target, ammo) changes,
   with how often the previous one repeated; `LogVerdicts` counts only those lines (budget restarts every
   battle). Stats every 10 s with deltas: picks that ran out (every try reached the solver and failed), how
   many of those B caused, weapons stopped / kept, all exits through the picker's clear, solver failures
   cached, unreachable volumes skipped, direct-control skips, new battles. B lines show the flag-1 threshold
   the engine actually compares pen6 with, the full-penetration armour, calibre vs the material's minimum,
   material flags, the resulting flags, the volume hit and the volume aimed at.

Review fixes (Nov 20 2026), also in the list above:
- **Direct control.** A unit under direct control is left alone by every hook (vanilla ammo choice, aim and
  fire for all its weapons, incl. the AI-run hull guns of a tank the player drives); `SkipDirectControl`.
  Test = the engine's IsManualControlled (game 0x804650, editor 0x67E5C0, server 0x617E70, by pattern):
  `[unit+0xDE0]+0x18 != 0` or bit 1 of `[[unit+0xDD8]+0x70]`, read with null checks.
- **New battle.** The sim clock starts at 0 again every battle and entity ids repeat, so every cache, memo and
  the log table is cleared the first time a hook sees the clock go back (as MeleeStance's mission flush).
- **Weapon in the key.** Holds, blocked and unreachable volumes are keyed by the ammo's name tags mixed with
  the weapon stuff's name tags (eStuffWeapon has eStuff at offset 0, so its names sit at +0x868 like the
  ammo's), so two mounts of one shooter with the same ammo but different `{parameters}` do not share verdicts.
- With `HookAimCheck=0` the solver call is still redirected while `HookAimPriority=1` (P's unreachable
  fallback needs the solver's failures); the record request and the armour check stay off then.

**About the "origin aim" from the triage:** read again, the engine clear (0x853240) sets the aim record
to mode 0, which every caller of the picker treats as "no aim" (eAttack 0x903BB6 and 0x95D543 then keep
their OLD aim; the weapon fire function 0x81F0C0 turns a mode-0 aim into a cease-fire). The target's
origin stays in the record's point field but nothing found fires at it. What does happen is that an
attack keeps its previous aim (the volume that just got blocked). So X keeps the engine clear (it must:
without it the record keeps the last blocked volume) and stops the weapon instead, only when the weapon's
own aim slots (weapon+0xC8 / +0x168) point at that target, so the enemy scan rejecting a candidate never
stops a weapon that is firing at something else.

## What is filtered

- Only ammo whose stuff `{tag}` list has `PierceTag` (`pierce`), the tag the engine's own
  penetration gate uses. HE, flame, smoke, gas, designators, rockets without `pierce`: untouched.
- Never ammo with any `ExemptTags` tag (`melee_light`, `melee_heavy`, `melee`: hand melee and the
  vehicle `*_melee_target melee` shells).
- Only vehicle targets: actors that are not humans (entity type byte +0x30: actor, not human). Infantry
  is never touched. Large fauna built as vehicles (Carnifex) count as vehicles.
- Friendly targets are left alone.
- "Can penetrate" = the engine's pierce flag (`PierceRule=1`: the round gets through the armour and
  `{on pierce}` fires; it may stop inside). `PierceRule=3` = full penetration only (what the direct-control
  sight overlay shows as pierced).
- P and B work with the weapon's LOADED ammo (what the shot will be); A with the ammo MatchAmmo is asked about.

## How

| Hook | Engine code (game exe) | What it does |
|---|---|---|
| W, ctx builder | 0x92C8E0 prologue (15 bytes) | Records which weapon the next MatchAmmo call is for (the enemy scan passes none: there the item's weapon stuff in r14 is used). |
| A1, rows with `{volume "x"}` | MatchAmmo 0x92C9C0, `call TagTest` at 0x92CC4A | REJECT verdict: no volume counts. ALLOW: a tagged volume counts only if the ammo can pierce it. |
| A2, rows without `{volume}` | MatchAmmo `test al,2; je match` at 0x92CBE3 | The row matches only on an ALLOW verdict. |
| P, aim priority | volume picker 0x92B6F0, try-loop head `mov rdi,[end]` at 0x92B8BA | Cuts the candidate vector (0x15B3630) to the reachable pierceable volumes of the best group; moves the picker's "previous volume" index with it (or drops it). Opens the per-pick state. |
| B, exact aim check | picker solver call at 0x92B9CD | The solver is asked for its hit record; on a hit the engine's armour chain runs on the real point / normal / face; no pierce = status 5 (blocked), the hit volume and the aimed volume (0x8527C0 on the aim record) are cached as blocked. Solver status other than 3 on a list P cut: the aimed volume is cached as unreachable. |
| X, pick ran out | picker `call Clear` at 0x92BA09 | Engine clear, then (only when the tries ran out and B blocked one): verdict worked out again and held, weapon stop 0x81F480(weapon, 1) if an aim slot holds that target and that aim cannot pierce (REJECT, or its volume blocked / failing the face test). |

Game direct-control test 0x804650 (IsManualControlled).
Editor sites: builder 0x7AACC0, A1 0x7AB02A, A2 0x7AAFC3, P 0x7A9BFA, B 0x7A9D0D, X 0x7A9D49 (clear 0x6CE920,
aim volume 0x6CDE20, weapon stop 0x699CC0, direct-control test 0x67E5C0).
Server sites: builder 0x73ED80, A1 0x73F0EA, A2 0x73F083, P 0x73DD5A, B 0x73DE6D, X 0x73DEA9 (clear 0x665300,
aim volume 0x664880, weapon stop 0x631D00, direct-control test 0x617E70).
All found by byte pattern / call chain at load time and cross-checked (`out\selftest.txt` has every
address). If anything does not match, that hook is not installed and the log says why. X needs P (the
per-pick state); P can run with `HookAimPriority=0` just for that.

### The picker (0x92B6F0), as read for P / X

Arguments: aim record, noAimCheck, weapon, target, filter Tag (the row's `{volume "x"}`, or empty), previous aim.
It collects every volume of the target that is unbroken, carries the filter tag and is not `no_target` into a
static vector; ebp = index of the previous aim's volume or -1. Then up to min(n, 5) tries: a random index (engine
RNG) unless ebp >= 0, set the volume, range check (out of range = straight to the clear), solver; status 3 = keep,
anything else = that candidate is erased and the next try rolls again. The solver (0x833C30 -> 0x835790) fails
for reasons that have nothing to do with armour: 1 no solution, 2 elevation limit (weapon stuff +0x15E8), 5 the
0x834D80 check, or the line-of-fire trace 0x836820 (terrain, buildings, other units). With noAimCheck set it takes
the first pick and returns (no range check, no solver, so no B). The clear (0x853240) at the end is also reached
with no candidates, no weapon and out of range; X only acts when all min(n, 5) tries reached the solver and B
blocked at least one of them.

Aim record: +8 mode (0 none, 1 entity, 2 point, 3 entity + point), +0xC point, +0x18 entity, +0x20 volume Tag
(name std::string +0x60). Weapon: +0x90 shooter, +0x98 weapon stuff, +0xA8 firer, +0xB0 ammo slot, aim slots
+0xC8 / +0x168.

### A / P pierce test

Per volume: penetration at the current shooter-target distance from the weapon's `{parameters}`
`projectileDamageTable` for that ammo (engine 0x841650) against the volume's thinnest face that faces the
shooter, using the engine's own thickness (0x7FC5B0), effective-armour (0x7FCA60) and pierce-threshold
(0x7FCDE0) functions, the calibre / material rules of 0x7FC3A0, the entity armour factor and actor multiplier.
A volume B blocked for this shooter / target / ammo + weapon fails while its blocked entry holds (see Caches).
- Box volumes: the faces turned towards the shooter (eye 1.5 m above the shooter's origin), each at its
  own incidence angle (bucketed to 1/32 of cos, rounded towards head-on).
- `.vol` polyhedra: their triangles carry a SIDE byte (front +X, top +Z, left +Y, right -Y, rear -X,
  bottom -Z: the same frame as box faces). Only sides clearly turned away (axis cos < -0.2) drop out;
  the others count head-on.
- Spheres, cylinders: all six faces head-on.
- Computed-thickness materials (material flag 4): left to hook B.
So the test is optimistic: it only rejects volumes no reachable face of which can be pierced; B and its
cache catch the rest (sloped faces, plates in front).

### Aim priority groups

`AimPriority` is matched against the volume TAGS (what `{volume "x"}` rows match too; `make_component`
gives a volume its component name as tag). Default, from the tags in the mod's vehicle files (Nov 2026):

| Group | Tags |
|---|---|
| 0 body | `body` (body..body25, standc / standl / standr), `cabin`, `bodyback` |
| 1 turret | `turret`, `mantlet`, `aux_turret1`..`aux_turret9` (sponsons) |
| 2 arms | `arm_left`, `arm_right`, `gun_left`, `gun_right`, `hkm_left`, `hkm_right` (dreadnought) |
| 3 legs | `leg_left`, `leg_right` (dreadnought), `frontleftleg` .. `backrightleg` (walker legs) |
| 4 tracks | `mover`, `track` (tracks define), `trackleft`, `trackright` (Taurox), `wheel` |
| last | everything else (`engine`, `dozer`, `shield`, `gun_shield`, untagged volumes) |

Barrels (`main_weapon` define: tags `no_target gun`), `armor` plates, `shield_track` and `solid_wheel` are
`no_target` and never candidates. `cabin` / `bodyback`, `mantlet` / `aux_turret*`, `hkm_*`, the walker legs
and `wheel` are my additions to Alex's body > turret > aux turrets > arms > legs > tracks order; change the ini if not wanted.

### Caches

Sim clock 0x1584568, never wall time; keyed by entity id (+0xC0), a hash of the ammo's name tags mixed with
the weapon stuff's name tags, and the volume's index in the target's list, so the same on every machine;
direct-mapped by those keys, so which entry a collision evicts is the same everywhere too.
- Blocked volumes (from B, hit and aimed): kept until the unit direction from the volume to the shooter's eye,
  in the volume's own frame (box / polyhedron volumes; others: world axes from the target's origin), turns by
  more than 25 degrees, or the shooter comes more than `HoldDistanceM` nearer, or `CacheMs` (60 s) runs out.
- Unreachable volumes (solver failures on a list P cut): `UnreachableMs`. Only P reads them; A and X never do.
- Ammo verdicts (from A, rewritten by X): `VerdictHoldMs`; REJECT dropped `HoldDistanceM` nearer, ALLOW
  `HoldDistanceM` farther, either when the penetration-range state (pen >= 1e-5) changed.
- Engine lookups (params, materials, angle states, face thresholds) are memoised; those memos are pure
  functions of their keys, so their eviction order cannot change a result.
- The change-only log table is log only; nothing in the simulation reads it.
- All of it is cleared when a hook sees the sim clock go back (new battle; MP: every machine sees that at the
  same sim point, and a machine that joined fresh had empty caches anyway).

**Not built: hook C** (approach into penetration range, eAttack range read 0x9034AA). `HookApproach=1` is
ignored with a log line.

## ini (`gem_penfilter.ini`, next to the dll)

| Key | Default | |
|---|---|---|
| Enabled | 1 | |
| Log | 1 | install lines, a stats line every 10 s while something changes (totals and +deltas), verdict lines |
| LogVerdicts | 400 | budget of verdict lines; a line is only written when a (hook, shooter, target, ammo) verdict changes |
| PierceTag | pierce | |
| ExemptTags | melee_light,melee_heavy,melee | any match = exempt |
| HookRowFilter | 1 | A (+ the builder hook W) |
| HookAimCheck | 1 | B |
| HookAimPriority | 1 | P cuts the candidate list; 0 = P only keeps the per-pick state for X |
| HookExhaust | 1 | X (needs P) |
| ExhaustStop | 1 | X stops a weapon whose aim at that target cannot pierce |
| HookApproach | 0 | not built |
| AimPriority | see above | groups `;`, alternatives `,` |
| VerdictHoldMs | 8000 | sim ms an A verdict is kept |
| HoldDistanceM | 15 | REJECT held / blocked volume dropped when the shooter comes this much nearer, ALLOW held dropped when it moves this much away (0 = never) |
| CacheMs | 60000 | safety limit (sim ms) for a B-blocked volume; it normally goes when the direction turns > 25 deg or the shooter comes nearer |
| UnreachableMs | 3000 | sim ms a volume the solver could not reach is skipped by P |
| PierceRule | 1 | 1 engine pierce, 3 full penetration |
| PlayerUnits | 1 | 0 = only AI-owned units are filtered (a human player's units keep vanilla ammo choice and aim) |
| SkipDirectControl | 1 | a unit under direct control is left alone by every hook; 0 = filtered like the rest |
| Facing | 1 | 0 = all faces head-on |
| BlockStreak | (gone) | ignored, logged |

Log: `gem_penfilter.log` (game), `_editor.log`, `_server.log` next to the dll. Lines:
- `A [ms] shooter target ammo d pen -> ALLOW / REJECT (why)` - why = the passing volume / face / threshold,
  or the nearest miss and how many volumes B has blocked, or `held since N ms`; `(was X, seen N times)`.
- `P [ms] ... -> aim group N (tags): k of n candidates kept[, previous volume dropped][, u pierceable but
  unreachable skipped]` or `no reachable pierceable candidate`.
- `B [ms] ... pen=.. pen6=.. vs flag-1 threshold .. (full-penetration armour ..) calibre .. min .. mat flags ..
  -> flags ..; hit vol#i(tags) face f cos c, aimed vol#j(tags) "name" -> PIERCE / BLOCK`.
- `X [ms] shooter target: pick ran out after n tries, k blocked by B -> ammo verdict .. held[, weapon stopped /
  kept]`; `X ... under direct control: left alone`.
- `new battle [ms]: sim clock went back ..., caches cleared`.
- `stats [ms]: ...` every 10 s.

## Test in the editor (in-house, no launcher change)

1. Start the editor through the Indomitus launcher, wait in the main menu.
2. Run `out\inject.bat` (= `gem_inject.exe gem_penfilter.dll`, the injector from gem_itempen).
3. `out\gem_penfilter_editor.log` must say `row filter (hook A) installed`, `aim check (hook B) installed`,
   `aim priority (hook P) installed` and `exhausted-pick hook (X) installed`.
4. AI autocannon / heavy bolter unit vs an AI heavy tank (Leman Russ, Baneblade), front-on: no fire with the
   pierce ammo (`A ... REJECT`); with a melee weapon it should switch to melee, without one hold fire / pick
   other targets.
5. Same unit vs a light tank / Sentinel / Taurox: it should fire at the body when the body can be pierced,
   else the turret, else tracks (`P ... aim group N` lines; `B` lines show the real face, threshold and flags).
6. The 0.1 Russ case (AP + HE vs the venerable dreadnought): AP / HE should change at most once per
   `VerdictHoldMs` (`A` lines), reloads should not repeat every second, and `X` lines should appear where
   picks ran out.
7. Same with the unit moved behind / beside the heavy tank: rear and side shots allowed where the numbers say
   so; moving more than 15 m re-decides at once.
8. Ordered attack (player's unit, PlayerUnits=1) on a tank it cannot pierce: watch whether it keeps
   re-firing after an `X ... weapon stopped` line (see risks).
9. AP unit vs a tank whose body is hidden behind a wall / ridge but whose turret shows: `P ... unreachable
   skipped` and an aim group further down; it should fire at the turret.
10. Take a filtering unit under direct control mid-fight: no `X ... weapon stopped` for it, `direct control
    skips` counts up in the stats; it fires what the player chooses.
11. Stop the test and start it again: the log should say `new battle`, and the first verdicts of the new run
    must not say `held since` a time from the old run.

## Multiplayer

Every machine and the dedicated server need it (the scan and the picker run in the lockstep simulation;
P changes the picker's candidate list and its "previous volume" index, so the engine's random roll lands
elsewhere; B changes which volume is kept; X stops weapons and writes verdicts that change ammo choice).
A machine without it would aim and choose ammo differently. Guga's launcher would have to add it to the
payload, including the server payload. No RNG of its own, no wall clock, no address-dependent behaviour in
the shared caches.

## Open risks

- Not run in game. The struct layouts (hit record, armour block, bullet state, aim record, weapon aim slots)
  are read from the sight overlay, HitVolume, the picker and the fire / stop code, not observed live.
- X calls the engine's weapon stop from inside the picker. That is the engine's own cease-fire (the fire
  function calls it for a mode-0 aim; it clears the weapon's aim slots, stops the firer, resets the burst
  state, tells the shooter and runs the engine's net / replay notification branch at 0xC2B740), but here it
  runs from a new place. `ExhaustStop=0` turns it off and keeps the rest of X.
- An attack order (eAttack) keeps its own aim record (order+0x40) and re-fires it when it decides to
  (0x904817 / 0x904C0D); after a stop it may start firing at the blocked volume again until its next
  successful pick. Not covered.
- P works with the weapon's loaded ammo. While another ammo is loaded (a reload pending) the list is left alone.
- Volume world matrix: read from volume+0x58+0x30 (what the box trace uses). If the engine updates it
  lazily, facing uses a stale pose for a moment.
- Eye height (1.5 m) and the 20 units per metre scale are assumptions (MeleeStance notes).
- The blocked-volume direction gate reads the same volume matrix as the facing test; for spheres / cylinders
  (no frame read) it uses world axes from the target's origin, so a target turning on the spot does not
  free those. A blocked volume is not re-tried while nothing moves, even if another point of it could be
  pierced (B judges one aim point); the 60 s safety limit re-tries it.
- A target whose pierceable volumes are all hidden (the solver reaches none) keeps an ALLOW verdict: P leaves
  the list as it is, B blocks the visible ones, X stops the weapon, but the unit may keep that target and not
  fire at it until something changes (vanilla would plink at the visible armour).
- Direct control: inferred that the manual-control property is sim state on every machine (MeleeStance uses
  the same test in the sim); if it were local to the driving player's machine, the skip would differ between
  machines in MP.
- New-battle detection is the sim clock going back. A previous battle whose last hook call came earlier than
  the new battle's first one (a test stopped within its first seconds) is not seen; its entries can then
  apply for the same entity ids until they age out (hold 8 s, unreachable 3 s, blocked 60 s).
- PlayerUnits=1 also affects a human player's units: an attack order with a heavy bolter on a tank it
  cannot pierce makes them hold fire instead of plinking.
- Taurox `turret_pod.vol` / `turret_rl.vol` have swapped SIDE bytes (notes, `file_formats.md`):
  facing on those two volumes can be wrong.
- Any other dll that patches the same bytes (none known; MeleeStance patches the MatchAmmo prologue and
  reads its range call, ItemPierce patches HitVolume / BulletStep) turns the matching hook off.

## Files

- `gem_penfilter.c` source, `build.bat` (zig from the launcher clone or `pip install ziglang`),
  `build.bat test` = `out\pf_test.exe` on the three exes, output in `out\selftest.txt`: every finder, a patch
  run on the mapped image (checks the written bytes of A1 / A2 / W / B / P / X), the asm stubs called the way
  the engine calls them, and on a faked engine: P (body / turret / tracks / nothing / infantry), P's unreachable
  fallback (solver failure -> next group, back after `UnreachableMs`, nothing recorded on an uncut list), B
  (plate hit first while aimed at the body: status 5, both cached, other weapon unaffected, next pick turret),
  the blocked cache gates (safety limit, one-way distance, shooter going round, target turning in the volume
  frame), the verdict hold (time, one-way distance, range state), A through the MatchAmmo stubs (hold, range
  state, weapon key, direct control), the new-battle flush, X (B-caused or not, weapon aimed or not, aimed at
  a pierceable / blocked / unknown volume, direct control, exhausted counter), the change-only log, the
  priority parser.
- `out\gem_penfilter.dll` + `gem_penfilter.ini` (ship these), `out\gem_inject.exe` + `inject.bat`
  (test only), `out\selftest.txt` (last self-test).
- 0.1 sources: session scratchpad `pf011\bak_0.1\` (not kept).
