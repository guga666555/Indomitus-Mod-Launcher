# GEM Overcharge 1.1 — ammo swap by unit tag

Module `gem_overcharge.c` of **`gem_weaponscript.dll`**; settings in the `[overcharge]` section of `gem_weaponscript.ini`. Shared code: `..\common\gem_common.h`. Launcher integration / install: `..\README.md`.

## What it does

While the firing unit carries the script tag **`overcharge`** (ini `OverchargeTag`; the mod's own button script
adds and removes it), its weapon fires the alternate ammo whose **set-file name tags** are the loaded ammo's plus
**`overcharge`** (ini `OverchargeSuffix`):

```
set\stuff\ammo\plasmagun_mag.ammo            ->  plasmagun_mag.overcharge.ammo
set\stuff\ammo\plasmagun_mag.chaos.ammo      ->  plasmagun_mag.overcharge.chaos.ammo
set\stuff\shell\plasma_huge.plasma.ammo      ->  plasma_huge.plasma.overcharge.ammo
```

Everything the engine keys on the loaded ammo follows: the weapon's `{parameters "overcharge"}` block (picked per
shot by the ammo's name / `{tag}`s), bullet stuff, velocity, tracer, penetration, damage, hit fx. Weapons whose ammo
has no sibling are left alone (logged once per ammo).

- **Persistent swap:** while tagged, the overcharge magazine stays loaded (item filling pointer); the impact code
  and the aiming display ask the weapon for its *current* ammo, so a swap that only lived inside the shot gave
  overcharge speed/tracer with standard penetration/damage. The first shot after the tag is gone puts the standard
  magazine back. The aiming readout shows the overcharge values only from the second shot (the first shot itself is
  overcharged). Rounds a partial reload returns to the pack come back as the overcharge item and fire as standard
  rounds when the unit is not tagged.
- **Round cost:** one magazine round per overcharge shot, like a standard shot. With the sibling loaded the engine's
  own consume step takes nothing, so the dll takes the round itself.
- **Vehicles:** a vehicle gun is owned by its gunner; when the gunner has no tag, the dll follows the `{if linked}`
  path (gunner → vehicle) and reads the vehicle's tags, so the mod's vehicle button tags the vehicle.

### Mod data rules (learnt the hard way)

- **The engine's stuff name lookup returns the FIRST registry entry (alphabetical file load order) whose name tags
  CONTAIN the query, not an exact match.** A base ammo must sort before every file whose name is a superset of it,
  and a variant must not be a subset of an earlier-sorting file, or the parser silently merges it into that stuff.
  Hence `plasmagun_mag.overcharge.chaos.ammo`, not `plasmagun_mag.chaos.overcharge.ammo`.
- Editor maps save unit inventories by stuff name: test on a freshly placed unit, never a saved one that fired
  overcharge rounds (it spawns with the overcharge magazine as its standard load).
- A custom ammo type has no `desc/ammo/<type>` label in the UI; the engine shows `desc/stuff/<name words>` instead.

Indomitus files: `set\stuff\ammo\plasmagun_mag.overcharge(.chaos|.nurgle).ammo`,
`set\stuff\shell\plasma_medium.plasma.overcharge.ammo`, `plasma_huge.plasma.overcharge.ammo`; buttons
`interface\action\overcharge.set` / `overcharge_vehicle.set`; the toggle, blow-up roll and fx are mod script.

## How it works

Hooks:
- `eBulletShooter::Launch` (vtable slot 11, takes only the shooter): resolves owner → tag (entity script tag
  container at entity+0x1D0) → weapon item (weapon vtable slot 3) → loaded ammo (item+0x50, rounds +0x58) →
  sibling (walk the global eStuff registry; name tags at stuff+0xB80); swaps, calls the original, takes the
  round. Cached per ammo (64 entries), re-validated by name on every hit (eStuffs are recreated per map load).
- `SpawnBullet` (shared with GEM Converge / GEM Guidance): makes sure the right magazine is loaded when a bullet is
  created, in case a `{SyncedProjectiles 1}` spawn ever runs outside Launch (in the editor it runs inside).

| What | game | editor | server |
|---|---|---|---|
| `eBulletShooter::Launch` | exe+0x8184D0 | exe+0x69B450 | exe+0x62DDB0 |
| `SpawnBullet` | exe+0x81A470 | exe+0x69D350 | exe+0x62FD50 |

Layout: stuff registry = global `{begin, end}` of eStuff* (exe+0x157B248 game), walked by the lookup that logs
"No Stuff found for tags"; eStuff+0x868 = name Tags object (vector +0xB80/+0xB88, 0x60-byte Tags, string at +0x40),
+0xEC8 = `{tag}` Tags (vector +0x11E0); entity tag container: self pointers +0x308/+0x310, Tag vector
+0x318/+0x320; linked: [entity+0x1C0] bit 28, [entity+0x560] → link, link+8 = vehicle.

## ini (`[overcharge]` section of `gem_weaponscript.ini`)

| key | shipped | default | meaning |
|---|---|---|---|
| `Enabled` | 1 | 1 | 0 = do nothing |
| `OverchargeTag` | overcharge | overcharge | unit / vehicle script tag |
| `OverchargeSuffix` | overcharge | overcharge | extra name tag of the alternate ammo |
| `OverchargeLog` | 10 | 10 | first N swap/shot lines |
| `Verbose` | 1 | 1 | log the lines above (`LogFile` / `Console` are in `[weaponscript]`) |

Log: `"plasmagun_mag ammo" -> "plasmagun_mag overcharge ammo"` per new ammo, then
`shot: magazine 10 -> 9 (engine took 0 in Launch, dll 1; 1 SpawnBullet call(s) inside Launch)`.

## Status

Feature tested in the editor in the old combined dll (0.4.5+): overcharge rounds penetrate and damage with the
overcharge values, all IG / chaos / nurgle plasma guns and both vehicle shells swap, one round per shot.
Not tested in multiplayer.
1.1 (module of gem_weaponscript.dll, launcher payload build): offline chain test only (`..\test`, incl. entries pre-patched by sibling payload dlls), not yet built on the PC nor run in game through the launcher.
