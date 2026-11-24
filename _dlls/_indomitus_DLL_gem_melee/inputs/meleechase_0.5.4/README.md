# GEM MeleeChase 0.5.4

Better chasing for melee fauna and vehicles (vehicle-brain units) in Call to Arms - Gates of Hell 1.067-1.069: game, editor and dedicated server
(`call_to_arms.exe`, `call_to_arms_ed.exe`, `call_to_arms_server.exe`). An Indomitus launcher payload DLL.

**Status (0.5.4, Nov 19 2026):** built. On game 1.069 (game 6AFC6D57, editor 6AFC6E14, server 6AFC6DAF) every finder
is found, including the 0.5.4 entity tag list (entity +0x4E8 on all three; `tests/check_entity_tags.py`). The priority
order was tested offline against fake entities. **0.5.4 not run in game yet.**

**0.5.4 changes:**
- The priority words `ai_prioritize_melee` / `ai_prioritize_ranged` also work as entity tags, so script can switch them
  at runtime. A tag overrides the def props. See "Priority props and tags" below.
- Ranged priority (tag or prop) no longer sets the approach range. The engine's own value is kept; it only counts
  weapons that can attack the current target, so a unit whose main gun is broken or useless against that target closes
  to what can still hit. Ranged priority still skips the melee chase and the hold-ranged-in-melee rule.

## What it does

### Humans: not touched (0.5.0)
- The fight-distance multiplier of 0.1-0.4 (`human_fight_distance`, `fight_distance_mult`: two
  `Human::eOutdoorsBattle` vtable slots and the `Order::eAttack` const read) is removed. Both ini keys are ignored.
- Why: infantry melee is GEM MeleeStance (melee stance, fight-range draw, charge, blow reach 45). Two DLLs
  moving the same hand-fight distance worked against each other (stance marines stood staring at enemies inside
  the tripled distance, Nov 7 2026), and the multiplier had been run at 1.0 since then.
- Every remaining hook skips human entities (`ENT_FLAGS & 4`): the vehicle tick, the approach order, the
  weapon-target hold. The "fight/distance" readers are still located, read-only, because one of them leads to
  `GetCurrentWeapon` and the weapon list that the vehicle part needs (`find_weapons`, was `find_human`).

### Vehicles and fauna: the approach follows a moving target
- **Vanilla behaviour:**
  - When the target of a vehicle-brain unit is out of weapon range, the brain gives itself one move order into a
    circle of 0.95 x weapon range around the target's position at that moment.
  - Until that move ends, the brain doesn't think again, so a running target is chased to where it was.
- **With MeleeChase:** for a unit with at least one melee-tagged weapon:
  - The running approach is checked every `repath_interval_ms` (500), or every `repath_interval_close_ms` (200)
    within `close_range_m` (10 m).
  - It is re-planned when the point it drives to is off by more than `repath_fraction` (25%) of the distance to the
    target, clamped to `repath_min_m`..`repath_max_m` (1..10 m).
  - **Close combat (0.2.0, fixes "a target backing away can't be caught"):**
    - The unit drives to `contact_factor` (50%) of its melee reach instead of braking at the edge.
    - It aims `lead_seconds` (0.6 s) ahead of the target's movement (at most `lead_max_m`, 6 m). Velocity is
      measured from the target's position over game time.
    - A target backing off faster than `retreat_speed_mps` (0.3 m/s) is pursued even while it is still inside weapon
      range, and the move that reaches it doesn't brake. Vanilla stands still until the target leaves range, then
      starts a new approach from a standstill.
    - The unit stops to fight only when the target is in reach and not getting away.
  - **0.3.0, chase directly:**
    - `chase_always=1`: a melee unit chases whenever the target is outside 90% of its reach. Melee unit here means
      melee weapons only, or `ai_prioritize_melee`. Vanilla stands still until the target leaves weapon range.
    - `chase_run_mode=2`: the chase sets the unit's movement mode to run. The vanilla vehicle AI keeps whatever mode
      the unit has, so fauna walked after running infantry.
    - In the 0.2.0 test log the fauna stayed about 6 m behind targets running at 4-6 m/s, which points to the
      walking pace.
  - With `smooth_repath=1`, a move ended for a re-plan doesn't brake first.
- **Tag the fauna melee guns.** The test is the item tag, and the fauna melee guns in
  `Indomitus_001\resource\set\stuff\gun\melee_*` don't carry it yet. Add `{tag "melee"}` to each, for example:

```
{from "pattern mgun"
	{tag "melee"}
	...
```

### Vehicles / fauna with melee AND ranged weapons: priority props and tags
Put one prop in the unit's def (`{props "... ai_prioritize_melee"}`), or give the unit the same word as an entity tag:

| Prop / tag | The AI... |
|---|---|
| `ai_prioritize_melee` | drives in to its melee reach (the largest range among its melee-tagged weapons). Ranged weapons keep firing on the way. |
| `ai_prioritize_ranged` | only closes to its ranged reach (largest range among the other weapons) and holds there. The melee weapon still hits whatever comes close. |
| neither | vanilla: the approach range comes from the first of its weapons whose target is out of range (place order in the def). |

- **Precedence (0.5.4), highest first:**
  1. entity tag `ai_prioritize_melee`
  2. entity tag `ai_prioritize_ranged`
  3. def prop `ai_prioritize_melee`
  4. def prop `ai_prioritize_ranged`
- So a tag always beats the def props, and on the same level melee wins.
- The props stay the default, so existing defs work as before. The tags are for changes at runtime, for example a
  dreadnought that loses its gun arm:

```
{tags add "ai_prioritize_melee"}       ; gun arm broken: charge
{tags remove "ai_prioritize_melee"}    ; repaired: back to the def prop
```

- The tag is read on every AI update, so adding or removing it acts at once. It is applied wherever the props are read:
  the approach range (`apply_priority`) and the "hold ranged fire in melee" check (a ranged-priority unit, by tag or prop,
  keeps all its weapons firing).
- Ranged priority from either source leaves the approach range to the engine.
- Entity tags are simulation state (`{tags add/remove}` runs on every machine), so the choice is the same on every
  machine of a network game.
- The tag list is found in the engine code at startup (see "Entity tags" under Engine facts). If it is not found, the log
  says `weapon priority entity tags OFF (entity tag list not found) - def props only`, and only the props count.
- How it works: the vehicle brain stores one "approach range" per update (brain+0x3D4, -1 = target in reach). Right
  before the AI acts on it, the DLL replaces it with the chosen reach, or -1 when the target is already inside it.
- The chase re-planning above still applies to the approach.
- Prop names can be changed in the ini (`prop_prioritize_melee` / `prop_prioritize_ranged`); the tags use the same names.
- To verify in game: whether the vehicle brain's idle step also runs while its guns are firing (it only runs when the
  brain has no move/AI order). If a melee-priority unit stands and shoots instead of charging, that is the reason.

### Only the melee weapon attacks in melee (0.4.0)
- `hold_ranged_in_melee=1`: once the target is inside melee reach (x `hold_ranged_reach_factor`), the ranged
  weapons of a melee unit hold fire, so it no longer shoots and strikes at the same time.
- Units with ranged priority (tag or prop, by the precedence above) are not affected.
- How it works:
  - The vehicle AI hands each weapon its target in the target evaluation (exe+0x989A80), with
    `Weapon::SetTarget` (exe+0x80A370).
  - The DLL redirects those calls. For a non-melee weapon of such a unit it calls the engine's own "clear target"
    (exe+0x80A730) instead.
  - weapon +0x90 is the owner entity.
- Player-forced targets (attack orders) take another path and are not changed.

### Which units
- The vehicle part applies to every unit that runs the vehicle AI (`Actor::eVS_Root`): fauna and vehicles alike.
- The test is only the `melee` tag on one of its weapons. Nothing checks `fauna_main`.
- Vehicles that also carry ranged weapons need `ai_prioritize_melee` (prop or tag) to charge.

### Performance (0.3.1)
- **Idle ticks:** every vehicle-AI unit's idle tick reads its weapon list (range getter plus tag test per weapon), its
  entity tags (0.5.4) and, when no priority tag is there, its def props. Each list is read once for both priority
  words. Since 0.3.1 the tag tests compare interned Tag ids (one 8-byte compare per tag), not strings.
  - The ids come from the engine's `Tag(const char*)`.
  - For the first 400 checks (with at least 6 hits) both methods run, and any disagreement switches back to names
    (logged).
- **Move orders:** while chases are running, every move order's update takes a lock and scans the chase list
  (small).
- **Re-paths:** the main cost, about 1-2 per second per chasing unit. The pathfinder runs on its own threads.
  Tune with `repath_*`.

## Install
1. `build.bat` builds `dist\gem_meleechase.dll` and copies the ini next to it.
2. `build.bat payload` also copies `gem_meleechase.dll` and `gem_meleechase.ini` into `launcher\payload\`; then rebuild the launcher.
3. The log is `gem_meleechase.log`, next to the extracted DLL
   (`%LocalAppData%\Indomitus Launcher\payload\<code>\`).

**Expected startup lines:**
- `humans not touched (0.5.0: fight distance multiplier removed, infantry melee = GEM MeleeStance)`
- `vehicle melee chase ON (...)`
- `melee close combat ON (goal radius 50% of melee reach, lead 0.60 s ...)`
- `melee chase always ON ...` and `chase run mode ON (movement mode 2 while chasing)`
- `weapon priority props ON (props "ai_prioritize_melee" / "ai_prioritize_ranged")`
- `entity tags: entity +0x4E8 (no_auto_repair test exe+0x..., HasTag exe+0x..., 1 site(s))` (finder line, 0.5.4)
- `weapon priority entity tags ON (entity +0x4E8; tag "ai_prioritize_melee" > tag "ai_prioritize_ranged" > prop ... > prop ...)`
- `ranged fallback ON (ranged priority + melee weapon -> melee priority while the usable weapons reach < 80% of the ranged reach)`

**What the log shows during play** (first few only):
- `melee unit ... approaches`
- `target goal moved X m - new path (... moving away at N m/s)`
- `target N m (moving away at N m/s) inside weapon range - chasing`
- `target in reach - stopping`
- `melee-priority unit ...: target X m, approach range ... -> melee reach`
- `unit ...: entity tag "ai_prioritize_melee" -> melee priority (def props: ai_prioritize_ranged)` (0.5.4, first 20
  units whose tag changes what the def props would give; a unit is logged once per result). Log only, with
  `log_chase=1`: a unit already logged costs a look at the logged list, no def scan; the def is read by name only
  (it does not touch the tag-id trust counters the decisions use); after 4096 def scans the line is off for the run.
- `ranged-priority unit ...: usable weapons reach X m < ranged reach Y m -> melee priority` (0.5.4 ranged fallback)

## Multiplayer
It changes AI decisions, so every player in a match needs the DLL. Decisions use only game state
and game time (no wall clock, no randomness).

**0.5.3 (Nov 15 2026, source only: the dll in `launcher\payload\` is 0.5.2 until it is rebuilt; not run in game).**
The target-velocity table is keyed by the unit's memory address, which differs on every machine and every run. Up to
0.5.2 a unit whose 16 slots were taken pushed another unit's sample out, even one still in use: that unit's target
velocity restarted from 0 (no lead, no "target is backing off") at random, and differently on each machine (from
about 500 units chasing at once in an offline replay of both versions on two address layouts; where the engine
simulates the chase on every machine that is a desync). Now a sample is only taken over when it is older than 3 s
of game time, which the code restarts anyway, so the velocities no longer depend on addresses at any unit count.
Below that count 0.5.2 and 0.5.3 give identical results. Also: an ini value too long for its buffer is now cut, not left
unterminated.

## Engine facts (1.067 game addresses; the finders derive them again on every start)

### Weapons
- The weapon in hand comes from `GetCurrentWeapon(entity+0xDD8 weaponry)` (exe+0x806340). It returns the current
  place's weapon, or else the first weapon of the list at weaponry+0x30..+0x38.
- weapon +0x98 is the item; the item's tags are the vector at item+0x11E0..+0x11E8 (0x60-byte Tags, name
  std::string at +0x40).
- This is the same read the AI's "can lie down" test does for `cannot_lie`.

### Fight distance (located only since 0.5.0, nothing here is hooked)
- Brain const getter: exe+0x9038A0 `(brain, Tag*, default)`. It returns metres x 20. A missing const logs
  "not declared" only when the default is 1e38.
- `eOutdoorsBattle`:
  - It caches the value at +0x498 on enter (slot 2).
  - Slot 3 (tick, exe+0x94A790) starts the fight: TryHandFight exe+0x94BBA0.
  - Slot 4 (exe+0x94A010) gives it up (distance > 2 x).
  - state +0x30 is the brain; brain +0x38 is the entity.
- `eAttack` reads the const at exe+0x8EDA69 (redirected until 0.4.0, untouched now).

### Brain update
- When the brain has no AI order (brain+0xF0) and no user order (deque size brain+0x88), it runs the state's
  slot 3.
- SetAIOrder = brain vt[13]: it finishes and deletes the old order.
- Order tick: Update(phase) with 0 = start, 1 = tick, 2 = stop. A nonzero result in phase 1 ends the order.

### Vehicle root state
- `Actor::eVS_Root` slot 3 is exe+0x990870.
- Its targets are records at brain+0x3A0, used when brain+0x1E0 == 1. Record pos is +0x28/+0x2C.
- brain+0x3D4 is the weapon range when the target is out of range, else -1.
- "approach" at exe+0x990F67 does `eMoveTask(circle{x,y,r = 0.95 x range})`, built by exe+0x8F3750.

### `Order::eMoveTask`
- Update is slot 19 (exe+0x8F6230). Phase 2 stops the chassis (exe+0x83FB00 on entity+0xDE0), then
  FinishSub (exe+0x901B00).
- The deleting dtor is slot 0.

### Weapon priority
- Vehicle brain update (eVehicleBrain vt[35]): the base brain update first (it runs the state tick), then the target
  evaluation (exe+0x989A80).
- The target evaluation resets brain+0x3D4 to -1. For each weapon slot and target, it picks a weapon
  (exe+0x98ABA0). The first weapon whose target is beyond its range (exe+0x80A910, world units) sets brain+0x3D4.
- The tick uses the value from the previous update, so the DLL overrides it right before the tick.
- Def props: entity+0x548 -> def, props vector at def+0x330..+0x338.

### Entity tags (0.5.4; 1.069 game addresses)
- The tags that `{tags add/remove}` changes are a vector of 0x60-byte Tags at entity+0x4E8..+0x4F0: the tag container
  at entity+0x1D0, its vector at container+0x318. Same offset on 1.067, 1.068 and 1.069 (game, editor, server).
- Found the way the engine's own vehicle-AI test "has tag no_auto_repair" reads them (game exe+0x9AEA49):
  - `lea rdx,"no_auto_repair" ; lea rcx,[TAG] ; call Tag::Tag(const char*)` builds the static Tag. The DLL requires
    the ctor to be the one it builds its interned ids with.
  - `mov rcx,[rax+0x38]` (brain -> entity) `; add rcx,0x1D0 ; call HasTag`, with `lea rdx,[TAG]` just before the
    `mov` (game, server) or just before the call (editor).
  - HasTag (game exe+0x1663C0) reads the vector at container+0x318/+0x320, steps 0x60, and compares with Tag==
    (`mov rax,[rdx] ; cmp [rcx],rax ; sete al ; ret`). So entity tags compare by interned id, like the item and def
    tags.
- `tests/check_entity_tags.py` runs the same patterns on the three exes. It checks the result against an independent
  route: the `{tags}` script action's container offset (`add rdx,0x1D0`) plus its helpers' vector read
  (`mov rbx,[rdx+0x318]`), as GEM SensorTags / TeamTags find it.

### Movement mode
- `Order::eMovementChange(pose, mode, wait)` is built by exe+0x8F3930. Its Start calls chassis(entity+0xDE0) vt[45]
  (pose) and vt[44] = +0x160 (mode).
- The player move action takes the mode from the table at .data 0x14148BEF0: {0,1,1,2,3,0,1,2}.
- The human battle state sets mode 2 on enter. The vehicle state never sets one.

### Firing on the move (not changed by the DLL)
- Humans fire while walking only with the item tag `shoot_walking` (player control) or `shoot_walking_ai`
  (AI) (exe+0x938910).
- The fauna melee guns carry only `shoot_walking_manual`. Whether vehicle-class units check the same tags is not
  confirmed.

### Simulation clock
The clock (ms) is the dword at exe+0x1556E48. The getter is exe+0x65E700; the quant is the next dword.

## Files
- `src/gem_meleechase.c` is the unity file.
- `src/mc/` holds:
  - `core.c`, `patch.c`, `crashlog.c`: from GEM Fauna 0.5.1.
  - `melee.c`: the finders and hooks.
  - `main.c` and `test_main.c`.
- `tests/run_offline_check.sh <game> <editor> <server>` runs the finders on the three exes, then a clang
  Windows-target check with `-Werror`.
- `python tests/check_entity_tags.py [<game> <editor> <server>]` (0.5.4, needs pefile; no arguments = the Steam
  install) checks the entity tag finder on the three exes against the independent `{tags}` action route.
  - Result on 1.069: `ALL OK`, entity +0x4E8 on game, editor and server.
