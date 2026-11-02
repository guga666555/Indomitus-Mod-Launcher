# GEM Guidance 1.0 — fire-and-forget lock-on + drift

`gem_guidance.dll` + `gem_guidance.ini`. Shared code: `..\common\gem_common.h`. Integration: `..\README.md`.

## What it does

- **`lock_on`** (ini `LockTag`) on the **weapon** + the vanilla tag **`atgm`** on its **ammo**: fire-and-forget.
  The engine already steers any round whose ammo carries `atgm` toward the gunner's *live* aim point (Bullet::Quant,
  10°/s, the CtA TOW / SACLOS code), so the missile goes wherever the gunner keeps looking, and the AI, which
  retargets right after firing, throws its missiles away. With `lock_on` the entity under the sight when the shot
  leaves (or the ground point if nothing is there) is locked: the missile keeps tracking that target (entity
  position + the hit offset, so it follows a moving vehicle) and the gunner may aim elsewhere. If the engine's
  guidance cannot run (owner dead, weapon dropped) the dll turns the velocity itself at `LockTurn` deg/s.
  A locked entity that disappears (vtable changed) leaves the missile flying to its last position.
- **`drift_<N>`** (1 … `DriftMax`) on the weapon: rounds veer off their line with a constant sideways pull, so the
  miss grows with distance: N = degrees off at 100 units, about 2N at 200. Each round draws its own strength (0..N)
  and direction from its launch geometry (hashed, so lockstep machines agree). Unguided rounds: the launch origin
  slides sideways every tick, so the engine's closed-form gravity is untouched. Locked rounds (the engine flies them
  from their current position): the lock point walks sideways by tan(angle) × distance flown instead.

```
{from "rocket_launcher"
	{tag "lock_on"}
	{tag "drift_1"}
	...
}
```

Indomitus tags: `lock_on` + `drift_1` on `set\stuff\gun\HKM_launcher` and `HKM_launcher_x2`; their ammo
`HK_missile.ap.ammo` carries `atgm`. 1° drift at 4400 units = up to 77 units off.

## How it works

Hooks:
- `SpawnBullet` (shared with GEM Converge / GEM Overcharge): for a `lock_on` weapon it calls the engine's aim-point
  raycast with the direct-control flag forced on (`[[owner+0xDD8]+0x70] |= 2`, so AI shots raycast too) and reads
  the first record of the engine's raycast result vector; the lock stays pending while the original runs.
- `Bullet::Launch`: binds the pending lock to each bullet created inside that SpawnBullet call (so a multi-shot
  weapon from GEM Converge locks every pellet).
- `Bullet::Quant` (per bullet tick): writes the locked point into the aimer's target point (`aimer+0x60`) for the
  duration of the engine call and restores it afterwards, so the engine's own guidance does the steering; drift for
  unlocked rounds.

Hit record (0x48 bytes, as the engine's own readers use it): `+0 int type` (1 = terrain, 4 = entity volume),
`+4 float distance`, `+8 float3 point`, `+0x18 entity*`; an entity is accepted when the type is not terrain, its
RTTI name resolves and the record point matches the aim point. Lock table 64 bullets, drift table 128.

Bullet layout (eBullet.cpp, identical in all three 1.067 exes): +0xA0 ammo stuff, +0xA8 int16 ticks since launch,
+0xAA flags (0x400 = done), +0xB0 owner, +0xC8 launch origin float3, +0xD4 velocity float3 (units per tick),
+0xE0 position float3, +0xEC |v|, +0x128 weapon stuff.

Finders: `Bullet::Launch` via the `Invalid bullet velocity - %g` assert; `Bullet::Quant` = the user of the global
`atgm` Tag that also calls Launch; `GetHeldItem`, owner +0x90, human part +0xDD8, control flag +0x70, aimer target
+0x60 and the hit-vector global parsed from Quant / GetAimPoint.

| What | game | editor | server |
|---|---|---|---|
| `Bullet::Launch(bullet, pos, vel, gravity)` | exe+0x8327B0 | exe+0x6B5DB0 | exe+0x647DC0 |
| `Bullet::Quant(bullet)` | exe+0x833DE0 | exe+0x6B73C0 | exe+0x6493F0 |

## ini (`[guidance]`)

| key | shipped | default | meaning |
|---|---|---|---|
| `Enabled` | 1 | 1 | 0 = do nothing |
| `LockTag` | lock_on | lock_on | weapon tag |
| `LockTurn` | 10 | 10 | fallback turn rate (deg/s) when the engine guidance can't run |
| `LockLog` | 10 | 10 | first N locks logged |
| `DriftMax` | 45 | 45 | largest N accepted in `drift_N` |
| `AllowMultiplayer` | 1 | 0 | 0 = no lock-on in network games (drift always applies) |
| `LogFile` / `Verbose` / `Console` | 1/1/0 | same | |

Log with `Verbose=1`: `lock:` (what was locked, number of hit records), `lock tick:` (engine guidance or dll
steering), `lock end:` (ticks flown, distance from the target, distance flown, drift offset).

## Status

Feature tested in the editor in the old combined dll: lock-on (HKM Taurox / Chimera: fire at a vehicle, aim away,
missile hits) and drift (`drift_1`: 14-unit offset over 2784 flown, hit). Not yet tested: the AI firing in a real
match, moving targets, whether the engine's 10°/s turn limit loses fast crossing targets.
This split build: offline chain test only (`..\test`), not yet run in game.
