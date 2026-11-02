# GEM BombBay 0.7 — Indomitus Launcher payload DLL

Bomb-bay airstrikes for Call to Arms: Gates of Hell 1.067 (game, editor and dedicated server exes).
A conquest/MP roster airstrike normally flies the plane over the click and the engine spawns ONE bomb
entity there. With this dll the plane opens its doors and drops its own loaded bombs from the bay,
staggered, carrying the plane's speed, released early so the stick lands centred on the click, at a
per-line altitude.

Indomitus: `ig_marauder_bomb` with `{action "airstrike:marauder_bomb_1250ms_35m"}`.

## 0.7 — what changed from the 0.6 VCOMP140 build

0.6 was the bomb-bay feature of GEM Converge compiled standalone (`-DCV_BOMBBAY_ONLY`) as a `VCOMP140.dll`
proxy dropped into `binaries\x64`. 0.7 is the same feature as a **plain DLL the Indomitus Launcher injects**:

| | 0.6 (`_bckp\0.6_vcomp_proxy`) | 0.7 (this folder) |
|---|---|---|
| file | `VCOMP140.dll` proxy (forwarded the OpenMP runtime, needed `proxy_exports.h`) | `gem_bombbay.dll`, no exports, no proxy |
| loading | by the game's import table from `binaries\x64` | by the launcher, `LoadLibrary` before the game's entry point |
| source | `gem_converge.c` (2280 lines, 4 features, build define) | `gem_bombbay.c` (~1060 lines, bomb bay only) |
| ini | `[converge]` | `[bombbay]`, same keys minus `BombFallback` |
| rounds | **bug**: the standalone build never ran the magazine finders (`find_weapon_item_slot`, `find_filling_offs`), so every bomb place read as `rounds -1` and no run could start | finders run; "rounds UNKNOWN" is logged if they fail |
| GetVelocity hook | answered the measured velocity for *any* entity during the release | only for the plane that is releasing |
| BombFallback | read, never used | removed (a bay-action name is no entity; nothing to fall back to) |
| trampolines | RWX | RX |
| logs | finder results | + `patched:` line per patched address (for the other payload DLLs' logs) |

Nothing in the engine-facing logic (finders, lead computation, release, altitude) was changed.

## Files

| file | what |
|---|---|
| `source\gem_bombbay.c` | the dll. Unity build, zig/mingw, imports KERNEL32 + ucrt only. `-DBB_TEST` builds the offline finder check instead. |
| `gem_bombbay.ini` | settings, next to the dll (payload folder). Sealed into the launcher. |
| `build.bat` | `build.bat` → `dist\gem_bombbay.dll`; `build.bat test` → `dist\bb_test.exe`; `build.bat payload` → copies dll + ini into `..\..\launcher\payload\`. Uses the repo's portable zig (`..\..\zig\zig.exe`), else zig on PATH, else `python -m ziglang`. |
| `_bckp\0.6_vcomp_proxy\` | the studied 0.6 drop (README, ini, `VCOMP140.dll`, `gem_converge.c`, `build.cmd`) |

Runtime: `gem_bombbay.log` written next to the dll at every launch
(`%LocalAppData%\Indomitus Launcher\payload\<fingerprint8>\gem_bombbay.log`).

## Build and ship

```
build.bat payload        (then build_zig.bat in the launcher repo root -> new dist\Indomitus Launcher.exe, new fingerprint)
```
Offline check after a game patch (no hooks installed, prints what every finder resolved):
```
build.bat test
dist\bb_test.exe "F:\SteamLibrary\steamapps\common\Call to Arms - Gates of Hell\binaries\x64\call_to_arms.exe"
dist\bb_test.exe "...\call_to_arms_ed.exe"
dist\bb_test.exe "...\call_to_arms_server.exe"
```
Every address is found at runtime by byte patterns / RTTI, nothing is hard-coded, so a game patch either
keeps working or logs `OFF (engine code not recognised)` and the airstrike stays vanilla.

## Payload coexistence (read this before adding more DLLs)

The launcher injects payload DLLs in alphabetical order: `gem_bombbay.dll` sorts **before**
`gem_breedtags`, `gem_fauna`, `gem_noprone`, `gem_scriptext`, `gem_teleport`, … so its finders run on a
pristine image. It leaves these patches behind, which the DLLs after it must tolerate (the NoProne 0.5
lesson: never locate a function by its first bytes):

* `.rdata`: `eAirstrike` vtable slot 3, `eBombDropOrder` vtable slot 19 → point into `gem_bombbay.dll`.
* `.text`: 14-byte `FF 25` jmp over the entry of `GetVelocity(entity, float3*)` (prologue was
  `40 53 48 83 EC 30 …`), `StrikeSetup A` and `StrikeSetup B`.

The log prints each as `patched: <what> = … exe+0x……`. If a later DLL's log says a GetVelocity or
StrikeSetup finder failed, this is why. Conversely, if BombBay's own `install_hook_at` finds an entry already
hooked it logs `(entry already hooked by another DLL)` and degrades (velocity / altitude stay vanilla).

## What the mod data must provide

1. **Roster line**: `{action "airstrike:<BombAction>[_<N>ms][_<N>m]"}` on the plane's units-set entry
   (`BombAction` = `marauder_bomb` in the ini). Tokens in any order: `_1250ms` = 1250 ms between
   bombs, `_35m` = fly at 35 m. The engine only requires the action to start with `airstrike`; the
   rest is normally an entity name to spawn — the dll intercepts it before that, so the name needs
   no entity. Any other airstrike name (`reconnaissance_50`, `sc_250_mp`, ...) is untouched vanilla.
2. **Bomb weapon place** on the plane def, name starting with `BombPlace` (`bomb1`), with a bomb
   weapon (`eBombShooter`, i.e. a `{weapon_bombing ...}` set with `{filling "ammo <bomb>" N}`) and
   `{barrels N}` + `{shooter "shooter_bb"}` → mdl bones `shooter_bb00..NN` = the release points.
   The number of bombs dropped = the loaded rounds (max 16 per place, up to 4 `bomb*` places).
   Optional: `{tag "bomb_interval_<ms>"}` on the weapon set = that plane's default stagger.
   The engine's real bomb release is used (`eBombShooter::Launch` per barrel), so the bomb entity,
   its damage, fall and impact fx are the ammo's own.
3. **Script events** on the plane (optional, for doors / visible bombs): `bomb_run` at the start of
   the run, `bomb_run_dropN` when bomb N (= barrel N−1) leaves the bay. Indomitus: prop
   `marauder_bomber` → block in `40k_airborne\airborne_main.inc` plays `bomb_open`, hides bone
   `bomb_0N`, closes 3 s after the last drop.
4. Nothing else. The engine's own `{on fire "bomb1"}` does not fire (the dll calls the shooter
   directly), hence the drop events.

## How it works (engine facts, 1.067)

* `Order::eAirstrike` is a sequence order: MoveTo(click) → `eBombDropOrder(name)` → MoveTo(exit) →
  destroy. Hooks are vtable-slot patches on the sequence tick (slot 3) and `eBombDropOrder::Execute`
  (slot 19); every order holds its carrier at +8 (entity = iface − 0xAF0), the drop order its target
  x/y at +0x60 and the name at +0x38 (`std::string`). The sequence's children are an MSVC
  `std::deque<Order*>` (seq+0x28).
* First tick: find the drop child, parse its name; if it is a bay action, find the plane's `bomb*`
  places (entity+0xDD8 weaponry → place vector → place name Tags +0x18 / weapon +0x2B8), the
  weapon's `eBombShooter` (RTTI `.?AUeBombShooter@Weapon@Actor@@`; a pointer field of the weapon
  whose +8 points back), the loaded rounds (weapon → eAmmo::GetItem slot → item filling +8), send `bomb_run`.
* Every tick: the plane's velocity is measured from its own position deltas (units/ms; the engine's
  `GetVelocity` returns 0 for planes). With `BombLead=1` the stick starts when the along-track
  distance to the click ≤ v·sqrt(2h/g)·BombVelocity% + v·interval·(n−1)/2 − v·BombLeadDelay
  (h = plane z − `GroundZ(click)`; engine time is ms, g = 0.0001962 units/ms²). Otherwise it starts
  when the engine's drop step fires (plane over the click).
* Release: shooter+0x10 = barrel index, `eBombShooter::Launch(shooter)` (vtable slot 11) spawns the
  loaded ammo entity at `shooter_bbNN` with `GetVelocity(owner)` as its velocity — the dll hooks
  `GetVelocity` and, for that plane only, answers with the measured velocity during the call.
  Rounds −1 on the magazine item, event `bomb_run_dropN`.
* `eBombDropOrder::Execute` state 0: when a bay run exists for the carrier it returns without the
  vanilla spawn (and starts the stick if the lead never triggered); a bay-action name on a plane
  without a usable bomb place also skips the spawn (nothing to spawn). The run is forgotten only
  after this step, or the engine would still spawn its bomb after an early-released stick.
* Altitude: every MP airstrike flies at one static float (40.0, `.data`) that the flight setup
  stores into the flight command (`movss [rax+0x4c],xmm6`). Both strike setups that call it are
  hooked; when the name is a bay action the float is set to `_<N>m` / `BombAltitude` around the
  original call and restored after. The vanilla `airborne.ext` `MaxAirborneAltitude 100` may cap
  values above 100 m.
* Host / single player only: orders do not exist on clients. `BombBayMultiplayer=0` keeps vanilla
  behaviour in network games (whether eBombShooter bombs replicate to clients is untested). The
  payload is sealed, so every peer started through the launcher runs the same dll and ini.

## Log lines of a good run

```
GEM BombBay 0.7 loaded into call_to_arms.exe (timestamp 6AB3DCC7, base ...) from ...\payload\<fp>
Launch = exe+0x..., SpawnBullet = exe+0x..., weapon->stuff @ +0x..., ...
stuff tags vector @ stuff+0x...
eAmmo::GetItem -> weapon vtable +0x...
weapon->ammo +0x..., item filling stuff @ item+0x... (rounds +0x...)
bomb bay: eBombShooter::Launch = exe+0x..., GroundZ = exe+0x..., GetVelocity = exe+0x..., gravity ...
bomb bay: airstrike altitude 40 m @ exe+0x148C7C0 (flight setup exe+0xB81FC0), StrikeSetup A found, B found
patched: eAirstrike tick = vtable slot exe+0x... (was  exe+0x...)
patched: eBombDropOrder::Execute = vtable slot exe+0x... (was  exe+0x...)
patched: GetVelocity = entry exe+0x... (14 bytes)
patched: StrikeSetup A = entry exe+0x... / patched: StrikeSetup B = ...
bomb bay airstrikes           ON (action "airstrike:marauder_bomb[_Nms][_Nm]", ...)
bomb bay: airstrike:marauder_bomb_1250ms_35m set up at 35 m (vanilla 40, setup B)
bomb bay: run started (airstrike:marauder_bomb_1250ms_35m), target (x y), bomb1 (rounds 4, shooter found), interval 1250 ms
bomb bay: early release 2080 units before the click (speed 0.598 units/ms, height 697, fall 2666 ms, carry 1593, stagger 1120, delay -598)
bomb bay: GetVelocity(...) during the release -> (0.597 0.026 -0.001) units/ms
bomb bay: bomb 1/4 released from barrel 00 at (x y z), 12.5 s after the run started
... bomb 4/4 ...
bomb bay: over the click after 16.7 s, 4 of 4 bombs out -> stick already running (vanilla spawn skipped)
```
`rounds UNKNOWN - magazine offsets not found` in the "run started" line means the eAmmo / filling finders
failed on this exe: no bombs are dropped (and nothing is spawned either); run `bb_test.exe` on the exe.
