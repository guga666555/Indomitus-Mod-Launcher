# GEM SquadFollow 0.2 (Indomitus Launcher payload)

Looser squad follow for Call to Arms: Gates of Hell 1.067 (game, editor, dedicated server).
`gem_squadfollow.dll` + `gem_squadfollow.ini` in the launcher payload.

## The problem
Squad members follow their leader with an engine check (`NeedRefollow`, game exe+0x951530).
Each brain tick it asks: has my formation slot moved more than ~6.3 units since my last move
order (and far enough compared to my distance to the leader)? If yes, the follower gets a new
move order. A running leader drags the slots along all the time, so followers get re-ordered
every few metres. Each new order restarts their movement, and their random run variant re-rolls.

## What the dll does
It replaces that one check with the same math and ini numbers:

| ini key | vanilla | default | meaning |
|---|---|---|---|
| `MinDrift` | 6.32 | 12 | slot must have moved this far since the last order |
| `DriftRatio` | 0.5 | 0.5 | and drift² > ratio × (slot-to-leader)² |
| `MinIntervalMs` | 0 | 600 | and this much game time since the last order (0..60000) |
| `ForceDrift` | – | 40 | past this drift the interval is skipped (sharp turns) |
| `Log` | – | 0 | 1 = 10 s stats, 2 = stats + every re-order |

`Enabled=0` gives vanilla behaviour. The time is the engine's own simulation clock (a global in ms
that the brain timers use; game exe+0x1556E48, the same clock GEM MeleeChase reads), so it is
deterministic. Nothing else in the engine is touched.

## Build (in this folder)
```
build.bat           dist\gem_squadfollow.dll + dist\gem_squadfollow.ini
build.bat payload   the above, then copies both into ..\..\launcher\payload\
build.bat test      dist\sf_test.exe on the three exes (folder: 2nd arg, %GOH_BIN%, or F:\SteamLibrary\...)
build.bat inject    dist\gem_inject.exe (in-house runtime test helper; not part of the payload)
```
zig: the launcher repo's `zig\zig.exe`, else zig on PATH, else `python -m ziglang`. Every run writes
`build_log.txt` next to `build.bat`. The flags `-mcpu=baseline -ffp-contract=off -fno-fast-math` keep
float results identical on every CPU (lockstep MP) - don't change them. `src\build.bat` just forwards here.

Linux offline check (same test code): `test/run_host_test.sh <call_to_arms.exe> <_ed.exe> <_server.exe>`.

## Launcher integration
- **Files:** `gem_squadfollow.dll` + `gem_squadfollow.ini`, side by side in `launcher\payload\`. The launcher
  embeds both, rewrites them to `%LocalAppData%\Indomitus Launcher\payload\<fingerprint>\` every launch and
  injects early-bird (before the game's entry point). Edit the ini **here** (`src\gem_squadfollow.ini`) and
  rebuild the launcher; the LocalAppData copy is overwritten. Because the ini is sealed in the launcher,
  every player on the same launcher build runs the same numbers (required for MP).
- **Log:** `gem_squadfollow.log` (`_editor.log` / `_server.log`) next to the dll. Expected on a good start:
  ```
  [GEM SquadFollow 0.2] call_to_arms_ed.exe | Enabled=1 MinDrift=12.00 ...
  [GEM SquadFollow] patched: NeedRefollow entry exe+0x7D8B50 (14-byte jmp, replaced, no trampoline), clock exe+0x13BE208
  [GEM SquadFollow] ON; simulation change: every player in a multiplayer match needs this dll with the same ini
  ```
- **Loading:** plain dll, no exports, no proxy, no thread. Everything happens in `DllMain`
  (DLL_PROCESS_ATTACH). It does nothing outside `call_to_arms*.exe` (workshop tool excluded), loads only once
  per process (a second copy, e.g. from `gem_inject`, logs "another copy is already loaded" and stays off),
  and pins itself after patching (it can never be unloaded).
- **Patch:** a 14-byte `FF 25 00000000 <abs64>` jump over the start of the 0x82-byte leaf function. The
  function is replaced, not wrapped: no trampoline, nothing to chain. No other payload dll touches it
  (BombBay: GetVelocity/StrikeSetup; WeaponScript: bullet hooks; Fauna: the *vehicle* follow handler's
  GetLeader calls - fauna follow does not go through NeedRefollow; MeleeChase: vehicle brain/eMoveTask).
  The finder is a byte pattern in `.text`; if it misses, the dll reads `call_to_arms*.exe` on disk and logs
  either "already changed in memory by another dll" or "unknown game build" - and patches nothing.
- **Payload order:** alphabetical, so it loads after scriptext and before teleport / vfs_redirect /
  weaponscript. It doesn't depend on load order.
- **Dependencies:** KERNEL32 + the UCRT api-sets (Windows 10+).

## Addresses (1.067, from `build.bat test` / `run_host_test.sh`)
| | game | editor | server |
|---|---|---|---|
| NeedRefollow | 0x951530 | 0x7D8B50 | 0x766510 |
| sim clock (uint32 ms) | 0x1556E48 | 0x13BE208 | 0x11A1328 |

Layout (read from the function bytes): brain+0x30 human, human+0x38 commander, commander+0x74/+0x78 pos,
human+0x138/+0x13C slot, brain+0x3CC/+0x3D0 slot at the last order.

## Changes in 0.2 (launcher adaptation)
- **MP determinism fix.** The per-brain stamp table is keyed by brain pointer. In 0.1 an entry not seen
  for >10 s kept its old stamp if its slot was still free, but became a fresh entry if another brain had
  reused the slot. Pointer values differ between machines, so two peers could answer differently for the
  same follower (e.g. a soldier that left "following" for a while and came back with the same order
  point) -> desync. Now an expired entry is always reset, so the answer depends only on game state and
  the game clock. The offline test has a case for it (0.1 logic: "slot kept 1, slot reclaimed 0").
- Table and stats under an SRW lock (harmless if brains tick on one thread, safe if not).
- Unicode ini/log paths (payload lives under the player's `%LocalAppData%`); ini values clamped.
- Exe filter, one-copy guard, self-pin, clearer NOT-installed reasons, MP log line.
- Build moved to the folder root with the launcher conventions (`payload`, `build_log.txt`, cache retry).

## Status
- Offline (2026-11-05, real 1.067 exes): finders unique in game / editor / server; vanilla equivalence
  0 / 2,000,000 per exe; `Enabled=0` = vanilla; interval and expiry cases OK. Windows-target compile with
  zig's mingw headers: 0 warnings (`-Wall -Wextra`).
- 0.1 was injected at runtime into the editor on 2026-11-04 and confirmed working (Alex).
- **0.2 not yet built on the PC nor run through the launcher.**

## Folder
```
build.bat                         build (see above; writes build_log.txt, NOPAUSE=1 skips the pause)
src\gem_squadfollow.c             the dll (+ offline test under -DSF_TEST)
src\gem_squadfollow.ini           payload ini (source of truth)
src\gem_inject.c                  in-house runtime injector (test only)
test\run_host_test.sh, host_map.c, stub\windows.h   Linux offline check
bin\, tools\                      the original 0.1 binaries (reference; not used by the build)
```
