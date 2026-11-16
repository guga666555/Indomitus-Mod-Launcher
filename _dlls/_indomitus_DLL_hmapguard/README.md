# GEM HeightmapGuard 0.1

Indomitus Launcher payload DLL (`gem_hmapguard.dll` + `gem_hmapguard.ini`), game 1.067 (game, editor, dedicated server).

Stops one engine crash: a crater (or any other land heightmap patch) landing on a terrain cell that still
lists a patch record whose memory was already freed.

**Status: offline-tested against the real engine code of all three exes. It has not fired in a game yet**
(the stale record cannot be produced on demand).

## The crash

- Nov 15 2026, Alex's hosted MP game (`multi/3v3_trenches:battle_zones`, launcher 0.9.14, payload 7417e14c),
  29 min in, on an SM macro barrage impact. The **dedicated server** died:
  `EXCEPTION_ACCESS_VIOLATION read`, `call_to_arms_server.exe+0x6998B4`, `[main_loop]`, no payload dll on the stack.
- Same engine code as KIBER's Oct 29 client crash (60mm rocket crater).
- `eVisibilityHeightmap` keeps a grid of 10-unit cells. Each cell has a list of the heightmap patches that
  touch it; a patch is a vector of 20-byte points. Two helpers walk every record of every cell in a rectangle:

  | helper  | server exe | game exe | editor exe | does |
  |---------|-----------|----------|------------|------|
  | restore | +0x6997E0 | +0x8848D0 | +0x708940 | saved bytes -> height grids |
  | apply   | +0x699980 | +0x884A70 | +0x708B00 | grids -> saved bytes, new heights in |

- In the dump the cell held one record whose point vector was garbage (begin `0x5a0ef004`, end `0xa80e8720`):
  freed and reused memory. The objects on the stack were the vanilla `crater_massive`'s
  `eLandPatchCutExtender` / `eLandHeightmapExtender`.
- Which engine path leaves the record behind is **not known**. This DLL does not fix that; it only keeps the
  walk from crashing.

## What the DLL does

It patches no engine code. It adds a first-chance vectored exception handler that acts on an access
violation **only** when the faulting instruction is inside the per-record body of one of the two loops:

- fault in the body -> execution continues at the loop's own "next record" step
- fault at that step itself (the list node is bad) -> continues at the loop's own "cell done" step

Both targets only use registers the body never changes, or reload from the stack. Any other exception,
and an access violation anywhere else, is passed on untouched. The engine only installs an
unhandled-exception filter, which runs after vectored handlers, so the DLL sees the fault first.

The loops are found by byte signature (exactly one match each is required, otherwise the DLL logs
`NOT installed` and does nothing).

## Limits

- A stale record whose memory is still readable never faults. The engine then copies garbage for that
  record, as it does today. The DLL does not see those.
- The skipped record's saved heights are not written back: one 10-unit cell of the visibility heightmap can
  keep slightly wrong values.
- Only these two loops are covered. Other code in the same module also walks 20-byte points
  (server exe+0x697E79, +0x6996E7, +0x6997C7); not looked at.
- No simulation code is changed, but the payload fingerprint changes like with any new payload file.

## ini

```
[HeightmapGuard]
Enabled=1        ; 0 = handler not installed
Server=1         ; 0 = leave the dedicated server alone
Log=1            ; one "skip:" line per skipped record
MaxLogLines=200
```

Log: `gem_hmapguard.log` / `gem_hmapguard_server.log` / `gem_hmapguard_editor.log` in the payload folder.
A banner with the found addresses on install, then for every skip:

```
skip: restore loop, fault at exe+0x6998B4 reading 0x5A0EF004 -> record skipped (exe+0x699916) | node ... | 1 in this loop
```

A `skip:` line where there used to be a crash is the in-game proof that it works.

## Build / test

`src\build.bat` (zig cc; set `ZIG_EXE` to a zig.exe or `pip install ziglang`):

- `build.bat` -> `out\gem_hmapguard.dll`
- `build.bat test` -> `out\hg_test.exe`, run on `call_to_arms.exe`, `call_to_arms_ed.exe` and
  `call_to_arms_server.exe`: maps the exe, finds both loops, then calls the engine's own restore / apply
  helpers on a hand-made one-cell grid with good records and three kinds of stale one (unreadable patch,
  unreadable points, unreadable list node), and checks that faults outside the loops are passed on.
  Result on 1.067: ALL OK on the three exes.

## Adding it to the launcher

Copy `payload\gem_hmapguard.dll` and `payload\gem_hmapguard.ini` into `launcher\payload\` and rebuild.
Nothing else: no launcher code change, no script change, no mod file change. Alex's test build is
0.9.14 (8d41693) + these two files.
