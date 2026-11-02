# GEM WeaponScript 1.1 — Indomitus Launcher payload build

**One dll, `gem_weaponscript.dll` + `gem_weaponscript.ini`**, holding the three weapon engine hooks for
**Call to Arms: Gates of Hell 1.067** (GEM engine) that were split out of the all-in-one *GEM Converge 0.5.1*
(`VCOMP140.dll` proxy) Indomitus used during development. 1.1 is that code adapted to the **Indomitus Launcher**
payload: no proxy loading, launcher build/copy flow, finders that survive the other payload dlls patching the same
engine code first, and the three modules packaged into a single dll.

| module (own source file, own ini section, own log tag) | feature | hooks |
|---|---|---|
| `gem_converge.c` — `[converge]` — `[GEM Converge]` | barrel convergence (`converge`) + multi-shot (`bullets_per_shot_N`) | SpawnBullet |
| `gem_overcharge.c` — `[overcharge]` — `[GEM Overcharge]` | ammo swap by unit tag (plasma overcharge) | eBulletShooter::Launch, SpawnBullet |
| `gem_guidance.c` — `[guidance]` — `[GEM Guidance]` | fire-and-forget lock-on (`lock_on` + `atgm`) + drift (`drift_N`) | SpawnBullet, Bullet::Launch, Bullet::Quant |

Each module is its own translation unit with its own copy of the finders and engine bindings, exactly as when they
were three dlls, and `Enabled=0` in its section switches it off alone. `gem_weaponscript.c` holds DllMain, the exe
filter, the one-copy mutex, the log file and the ini, and calls `converge_init()`, `overcharge_init()`,
`guidance_init()` in that order; a module that is OFF leaves the engine alone (any hook it had already placed
passes straight through) and never touches the others. Once a module is ON the dll pins itself in the process. Game, editor and dedicated server
exes are supported (`call_to_arms.exe`, `call_to_arms_ed.exe`, `call_to_arms_server.exe`); every engine address is
found at run time from code patterns, nothing is hard-coded.

## Folder layout

```
README.md                  this file (launcher integration)
build.bat                  builds the dll; "build.bat payload" copies dll + ini into launcher\payload\
build_log.bat              same as build.bat but writes everything to build.log (for reporting a build problem)
gem_weaponscript\          gem_weaponscript.c (DllMain, log, ini, module init), gem_weaponscript.ini (all sections)
common\gem_common.h        shared module code: log/ini wrappers, PE + pattern helpers, exe-file readers, .pdata,
                           chainable hooks, shared engine finders
gem_converge\              gem_converge.c + README.md       (module)
gem_overcharge\            gem_overcharge.c + README.md     (module)
gem_guidance\              gem_guidance.c + README.md       (module)
test\chain_test.c          offline finder + hook-chain test of the three modules (Windows and Linux)
test\run_test.sh           Linux/WSL runner for it
test\stub\                 fake Win32 headers for a host-side syntax check
out\                       gem_weaponscript.dll + chain_test.exe
```

## Install (Indomitus Launcher)

1. `build.bat payload` — builds `out\gem_weaponscript.dll` and copies it with `gem_weaponscript.ini` into
   `..\..\launcher\payload\`. Uses the launcher's portable zig (`..\..\zig\zig.exe`), else zig on PATH, else
   `python -m ziglang`. If the three 1.0 split dlls (`gem_converge.dll`, `gem_overcharge.dll`, `gem_guidance.dll`)
   or their inis are still in `launcher\payload\` (or in `out\`), delete them: the dll detects a loaded split dll
   and does nothing.
2. Run `build_zig.bat` in the launcher folder: the payload is sealed into `Indomitus Launcher.exe` (SHA-256 manifest,
   new fingerprint).
3. Start the game or the editor through the launcher. The launcher extracts the payload to
   `%LocalAppData%\Indomitus Launcher\payload\<code>\` and injects every dll before the engine runs (early bird, in
   alphabetical order: breedtags, fauna, meleechase, noprone, scriptext, weaponscript, …).
4. Check `%LocalAppData%\Indomitus Launcher\payload\<code>\gem_weaponscript.log`: it must contain the three
   `… ON (…)` lines (tagged `[GEM Converge]`, `[GEM Overcharge]`, `[GEM Guidance]`), each module's `simulation
   change: every player of a multiplayer match needs this DLL` (or, for converge with `AllowMultiplayer=0`,
   `network games: standing down`), and end with `modules: convergence / multi-shot ON, overcharge ON, lock-on / drift ON`.

**Remove the old all-in-one dll.** If the old GEM Converge `VCOMP140.dll` proxy (≤ 0.5.1) is still in
`binaries\x64`, the dll detects it, logs it and does nothing (otherwise each effect would run twice). That old
dll also loads for Steam-started games, which the launcher design wants vanilla. It also carries the **bomb bay
airstrikes** feature, which is not in this split (see `claude/gem-bombbay-0.7-payload-handoff.md` for its own dll).

## What the launcher adaptation changed (1.0 → 1.1)

- **One dll instead of three.** `gem_weaponscript.c` is the process-level part (DllMain, exe filter, mutex, log,
  ini); each former dll became a module compiled into the same dll (`converge_init` / `overcharge_init` /
  `guidance_init`). Module statics stay per translation unit, so the finders, bindings and hook stubs are exactly
  the 1.0 ones; the hooks chain between modules as they chained between dlls. One log (`gem_weaponscript.log`, lines
  tagged per module) and one ini (`gem_weaponscript.ini`, `[weaponscript]` + one section per module).
- **No proxy, payload only.** Plain dll, nothing exported that the game needs (only `GemHookVersion`), all work in
  `DllMain(DLL_PROCESS_ATTACH)`. Process filter (`call_to_arms*` only), per-process mutex, `Enabled=0` guards
  (dll-wide and per module) kept.
- **Finders no longer trust the first bytes of a function or the vftable slot in memory.** Other payload dlls patch
  function entries (BreedTags / NoProne: 14-byte `FF 25` jump, ScriptExt: 5-byte `E9` jump) and swap vftable slots
  (ScriptExt) — that is what switched NoProne 0.4 off in every launcher run. 1.1 therefore:
  - reads a function's original first bytes from **`call_to_arms*.exe` on disk** (`orig_bytes`), falling back to the
    GEM stub chain when the file cannot be opened;
  - takes function starts from the exe's **unwind table (`.pdata`)**, never by walking back for a prologue
    (`func_start_of`; chained and indirect entries followed) — Bullet::Launch and Bullet::Quant;
  - when a vftable slot points outside the exe (swapped by a sibling), reads the slot's original value from the exe
    file and hooks **that** function (`vt_slot_orig`) — eBulletShooter::Launch, eAmmo::GetItem, eCondition_Linked;
  - starts pattern scans past any entry patch, GEM or foreign (`hk_body`).
- **Chaining onto a foreign 14-byte jump** is explicit and logged (`chained onto an earlier hook (another dll's 14-byte
  jump)`); a foreign 5-byte jump cannot be chained and is refused with a log line (that function's feature switches
  off, the others stay on).
- **Patches are written with every other thread suspended** and refused if a thread sits inside the bytes to patch
  (same rule as BreedTags / NoProne). Irrelevant under early-bird injection (no other thread yet), it matters for the
  launcher's watch mode / CreateRemoteThread fallback. No allocation or logging while threads are suspended.
- **Multiplayer log line** on every ON feature (`simulation change: …`), as in NoProne. The launcher ships the same
  payload to every player; a Steam-started game is vanilla.
- **Test build runs on Linux too** (`-DGEM_TEST` uses no Win32 API), so `test/run_test.sh` can run where the exes are
  readable; `build.bat test` is the Windows equivalent. The chain test links the three modules without
  `gem_weaponscript.c` and runs their finders/hooks in all 6 orders; it also runs a second scenario with the four
  hooked entries pre-patched by a foreign 14-byte jump and the Launch vftable slot swapped away.
- Log header names the exe's base address, whether an unwind table was found, and the payload folder.
- The ini header says the file is sealed into the launcher (`build.bat payload` → `build_zig.bat`).

Feature code is unchanged from 1.0 (which was the 0.5.1 code moved across). Version strings are 1.1.

## Hook chaining contract

All three modules hook `SpawnBullet`, so the framework in `common\gem_common.h` (`install_hook_at`) makes inline
hooks chain instead of fighting (this is also what lets other payload dlls, present or future, share those
functions):

- The hooked function's first bytes become `FF 25 00000000 <stub>` (absolute 14-byte jump) + NOP padding up to the
  end of the instructions it replaced.
- The stub, in the hooking dll's own memory: `FF 25 00000000 <hook>` (14 bytes), `"GEMHOOK1"` (8), `u8 n` (bytes
  patched), then the `n` bytes that were there before. At stub+64: the trampoline (those bytes + a jump back to
  function + n), which the hook calls as "the original".
- A later dll hooking the same function treats the earlier 14-byte jump as one position-independent instruction:
  it copies it into its own trampoline and patches over it. Calls run newest hook → … → oldest hook → original.
  The same works for a 14-byte jump written by another payload dll (BreedTags style): it is chained onto, and the
  dll identifies the function through the exe file instead of the stub chain.
- The combined order on SpawnBullet does not change behaviour: GEM Converge multi-shot calls the next hook once per
  pellet, GEM Guidance binds its pending lock to every bullet created inside its call, and GEM Overcharge only makes
  sure the right magazine is loaded before each bullet is built.

## Build and post-update check

`build.bat` / `build.bat payload` / `build.bat test` (zig's clang + mingw-w64, no Visual Studio). After a game
update run the chain test on each exe:

```
out\chain_test.exe "F:\SteamLibrary\steamapps\common\Call to Arms - Gates of Hell\binaries\x64\call_to_arms.exe"
```

(or on Linux/WSL `test/run_test.sh <exe> ...`). It maps the exe at its base (nothing is executed) and, for all 6 module
orders, runs each module's finders and hooks on that copy in turn, then walks every patched function's stub chain; then
repeats the 6 orders on a copy whose Launch / SpawnBullet / Bullet::Launch / Bullet::Quant entries carry a foreign
14-byte jump and whose eBulletShooter slot 11 is swapped away. It must end in `ALL OK` for the game, editor and
server exe; a failing finder prints which pattern it lost.

## Status (1 Nov 2026)

- 1.1 passes the offline chain test on game `6AB3DCC7`, editor `6AB3DD83` and server `6AB3DD21`: 12/12 module orders
  per exe (6 clean + 6 pre-patched), SpawnBullet chained 3 deep, every chain ends in the original bytes / the foreign
  jump, all addresses identical to the 1.0 tables in the per-module READMEs.
- The three-dll form of 1.1 was built with the launcher's zig on the PC (1 Nov 2026); the single-dll form compiles
  clean for `x86_64-windows-gnu` against zig's own mingw headers but **has not yet been built on the PC nor run in
  game through the launcher**. The features themselves were tested in the editor in the old combined dll (see each
  README).
- Reviewed 1 Nov 2026 (own pass + an independent read): fixed the trampoline pointer being published after the jump
  went live, guidance acting (drift) while reporting OFF after a partial hook install, a thread exiting during the
  patch aborting it, unbounded path building and silent ini truncation in the core, `rva_to_raw` bounding by raw
  size only, the stub leak on a refused patch; the dll now pins itself once a hook is in. Engine facts noted by the
  review: the 1.067 exes have no DYNAMICBASE (fixed base 0x140000000) and ~39 % of the game exe's `.pdata` entries
  are chained, so `pdata_entry`'s chain walk matters.
- Open: the Linux/host build cannot exercise `patch_code` (thread suspension) — the first launcher run should show
  the three ON lines with no `could not suspend` line.
