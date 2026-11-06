# GEM Converge 1.1.1 — barrel convergence + multi-shot

Module `gem_converge.c` of **`gem_weaponscript.dll`**; settings in the `[converge]` section of `gem_weaponscript.ini`. Shared code: `..\common\gem_common.h`. Launcher integration / install: `..\README.md`.

## What it does

Per-weapon opt-ins, stuff tags on the gun's set file:

- **`converge`** (ini `Tag`): every barrel of a multi-barrel weapon fires at the point the gun is aimed at. Without
  it the engine fires all barrels parallel to the single `foresight` bone (the `shooterNN` bones only set where
  each round starts), so twin guns never converge.
- **`bullets_per_shot_<N>`** (2 … `BulletsPerShotMax`): one trigger pull spawns N real engine bullets spread
  uniformly inside a cone (`bullet_spread_<deg>` on the weapon, 1–45, else ini `BulletSpread`, default 3° half-angle).
  Ammo is still one round per pull; every pellet carries the ammo's full damage/penetration (tune the ammo to one
  pellet's worth). Sound, muzzle fx and casing are per pull, hit fx/decals per pellet.

```
{from "heavybolter_x2"
	{tag "converge"}
	...
}
```

Indomitus tags: `converge` on `tarantula_heavybolter_x2`, `tarantula_multilaser_x2`, the two `tarantula_chaos_*`
guns and `ig_autocannon_x4` (all `set\stuff\gun\`). No weapon uses `bullets_per_shot_N` at the moment.

## How it works

`eBulletShooter::Launch` (eWeaponShooterBullet.cpp, vtable slot 11) builds one direction from the aimer
(foresight X axis + spread + zeroing) and calls `SpawnBullet(shooter, syncIdx, float3 *zero, float3 *dir)` once
per barrel, which takes the barrel's `shooterNN` bone position as the start and launches `dir`.

The hook wraps `SpawnBullet`. For a `converge` weapon it asks the engine's own aim-point getter
(`GetAimPoint(aimer, double3*)`: raycast along the foresight for the gun's range under direct control, else the
aimer's cached target point) and rotates `dir` by `normalize(aimPoint − shooterPos) − normalize(foresightDir)`.
Spread and zeroing survive; only the parallax between barrel and sight is removed. Below `MinDistance` nothing
changes. The correction is applied only while the sight itself is within `MaxAngle` of the aim point: under direct
control the aim point is where the gun is *commanded* to point (the cursor) and the engine fires along the
foresight while the turret traverses, so without this test a shot fired mid-traverse was turned all the way to the
cursor (1.1.0, seen 140° off the barrels). Those shots now go where the barrels point, as in vanilla, and are
logged as `sight N deg off the aim point ... no correction`. For multi-shot it calls the next hook / the original N times with directions scattered over the cone.

Pellet offsets are not random numbers: they are hashed (splitmix64) from the shot's own direction and aim point,
so every lockstep machine computes the same cone. On a host spawning a `{SyncedProjectiles 1}` weapon the extra
pellets take fresh ids from the engine's projectile sync counter (if that counter is not found, synced weapons
fire a single bullet).

## ini (`[converge]` section of `gem_weaponscript.ini`)

| key | shipped | default | meaning |
|---|---|---|---|
| `Enabled` | 1 | 1 | 0 = do nothing |
| `Tag` | converge | converge | convergence stuff tag |
| `MinDistance` | 3 | 3 | no correction when the aim point is closer (world units, 20 = 1 m) |
| `MaxAngle` | 10 | 10 | the sight must be within this many degrees of the aim point (else the turret is still turning: no correction) |
| `AllowMultiplayer` | 1 | 0 | 0 = stand down in network games |
| `BulletSpread` | 3 | 3 | default cone half-angle (degrees) |
| `BulletsPerShotMax` | 32 | 32 | largest N accepted |
| `Verbose` / `LogShots` | 1/10 | same | log the first N affected shots (`LogFile` / `Console` are in `[weaponscript]`) |

Log (`gem_weaponscript.log`, lines tagged `[GEM Converge]`): `barrel convergence / multi-shot   ON (...)` at start-up; with `Verbose=1` the first `LogShots` lines
`shot: bone shooter01, aim point 412.3 away, converged 0.73 deg, pellets 0 (cone 0.0 deg)`.

## Engine facts (1.067; the finders re-derive them)

| What | game | editor | server |
|---|---|---|---|
| `eBulletShooter::Launch` (vftable slot 11) | exe+0x8184D0 | exe+0x69B450 | exe+0x62DDB0 |
| `SpawnBullet(shooter, int, float3*, float3*)` | exe+0x81A470 | exe+0x69D350 | exe+0x62FD50 |
| `GetShooterBone(shooter, bone**, std::string*)` | exe+0x819EA0 | exe+0x69CE00 | exe+0x62F780 |
| `GetAimPoint(aimer, double3*)` | exe+0x81DB10 | exe+0x6A0B40 | exe+0x6333F0 |
| bone world-matrix refresh | exe+0x5C2690 | exe+0x439610 | exe+0x3DE130 |

- shooter+0x8 = weapon; weapon+0x90 = owner entity, +0x98 = stuff (set), +0xA8 = aimer, +0xB0 = ammo, +0xC0 = place.
- stuff+0x11E0/+0x11E8 = `{tag}` vector (0x60-byte elements, MSVC `std::string` at +0x40, lowercase); stuff+0x15F4 = SyncedProjectiles.
- bone+0x10 bit 0 = world matrix stale (refresh first); float 3x4 at bone+0xA8 (X axis) … +0xCC (position).
- aimer vftable: slot 4 range, 5 speed, 6 position (double3*), 7 direction (double3*); aimer+0x20 = aim (foresight) bone, aimer+0x60 = cached target point.

## Status

Feature tested in the editor in the old combined dll: convergence (Tarantula), multi-shot (pump shotgun,
8 pellets). 1.1 (module of gem_weaponscript.dll, launcher payload build): offline chain test only (`..\test`, incl. entries pre-patched by sibling payload dlls), not yet built on the PC nor run in game through the launcher.
