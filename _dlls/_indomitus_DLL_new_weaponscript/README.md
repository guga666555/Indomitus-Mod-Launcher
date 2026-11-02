# GEM hook dlls (split build 1.0)

Three independent engine-hook dlls for **Call to Arms: Gates of Hell 1.067** (GEM engine), split out of the
all-in-one *GEM Converge 0.5.1* (`VCOMP140.dll` proxy) that Indomitus used during development.

| dll | feature | hooks |
|---|---|---|
| `gem_converge.dll` | barrel convergence (`converge`) + multi-shot (`bullets_per_shot_N`) | SpawnBullet |
| `gem_overcharge.dll` | ammo swap by unit tag (plasma overcharge) | eBulletShooter::Launch, SpawnBullet |
| `gem_guidance.dll` | fire-and-forget lock-on (`lock_on` + `atgm`) + drift (`drift_N`) | SpawnBullet, Bullet::Launch, Bullet::Quant |

Each one works alone, in any combination, in any load order. Each has its own README, ini and log.
Game, editor and dedicated server exes are supported (`call_to_arms.exe`, `call_to_arms_ed.exe`,
`call_to_arms_server.exe`); every engine address is found at run time from code patterns, nothing is hard-coded.

## Folder layout

```
README.md                  this file (integration notes)
build.bat                  builds all three (+ "build.bat test" -> the offline test)
common\gem_common.h        shared code: log/ini, PE + pattern helpers, chainable hook framework, shared finders
gem_converge\              gem_converge.c, gem_converge.ini, README.md
gem_overcharge\            gem_overcharge.c, gem_overcharge.ini, README.md
gem_guidance\              gem_guidance.c, gem_guidance.ini, README.md
test\chain_test.c          offline finder + hook-chain test
out\                       built dlls + chain_test.exe
```

## Integrating with a launcher

- **Plain dlls, no proxy.** They export nothing the game needs (only `const char *GemHookVersion(void)`, e.g.
  `"GEM Converge 1.0"`) and do all their work in `DllMain(DLL_PROCESS_ATTACH)`. They must be loaded into the game
  process by the launcher (e.g. `LoadLibraryW` through `CreateRemoteThread`).
- **Load them before the engine runs game code.** The hooks overwrite the first 14–19 bytes of engine functions.
  Start the exe with `CREATE_SUSPENDED`, inject the dlls, then `ResumeThread`. Injecting into a running game
  is a race (a thread could be inside a prologue while it is rewritten); it is only likely to survive at the main
  menu, before any map with bullets in flight.
- **ini and log sit next to each dll** (`<dll folder>\gem_x.ini`, `gem_x.log`, found through the dll's own path).
  Missing ini = built-in defaults (note: the defaults are not always the values Indomitus ships in the ini, e.g.
  `AllowMultiplayer` defaults to 0, so ship the ini files).
- **Process filter:** a dll does nothing unless the exe name starts with `call_to_arms`, so loading it into
  another process (or `LoadLibrary` in the launcher itself, e.g. to call `GemHookVersion`) is harmless.
- **One copy per process:** a named mutex (`Local\gem_x_<pid>`) makes a second injected copy do nothing.
- **Remove the old all-in-one dll.** If the old GEM Converge `VCOMP140.dll` proxy (≤ 0.5.1) is still in
  `binaries\x64`, every split dll detects it, logs it and does nothing (otherwise each effect would run twice).
  That old dll also carries the **bomb bay airstrikes** feature, which is **not** in this split (see below).
- **Multiplayer:** unsynced weapons are simulated on every machine, so `gem_converge` and `gem_guidance` need to be
  on every player's machine (their effects are deterministic, so lockstep copies agree). `AllowMultiplayer=0` in
  their ini makes them stand down in network games instead. `gem_overcharge` has no multiplayer switch and has not
  been tested in a network game.
- **Proxy names already taken** in the Indomitus dev setup, if anyone wants a proxy loader instead:
  `dbghelp.dll` (GEM BreedTags), `version.dll` (GEM Editor+), `VCOMP140.dll` (GEM NoProne / old GEM Converge).

## Hook chaining contract

All three hook `SpawnBullet`, so the framework in `common\gem_common.h` (`install_hook_at`) makes inline hooks
chain instead of fighting:

- The hooked function's first bytes become `FF 25 00000000 <stub>` (absolute 14-byte jump) + NOP padding up to the
  end of the instructions it replaced.
- The stub, in the hooking dll's own memory: `FF 25 00000000 <hook>` (14 bytes), `"GEMHOOK1"` (8), `u8 n` (bytes
  patched), then the `n` bytes that were there before. At stub+64: the trampoline (those bytes + a jump back to
  function + n), which the hook calls as "the original".
- A later dll hooking the same function treats the earlier 14-byte jump as one position-independent instruction:
  it copies it into its own trampoline and patches over it. Calls run newest hook → … → oldest hook → original.
- Finders that need a hooked function's original first bytes read them back through the stub chain
  (`hk_original`), and pattern scans start past the patch (`hk_body`), so a dll finds its code no matter which
  siblings loaded first.
- A jump at a function start that does not lead to a `GEMHOOK1` stub (another tool's hook) is chained onto, but
  identification by prologue then fails and that feature switches itself off with a log line.

The combined order on SpawnBullet does not change behaviour: GEM Converge multi-shot calls the next hook once per
pellet, GEM Guidance binds its pending lock to every bullet created inside its call, and GEM Overcharge only makes
sure the right magazine is loaded before each bullet is built.

## Build and post-update check

`build.bat` (needs `pip install ziglang`; zig's clang + mingw-w64, no Visual Studio) writes the three dlls to
`out\`. `build.bat test` builds `out\chain_test.exe`; run it on each exe:

```
out\chain_test.exe "...\binaries\x64\call_to_arms.exe"
```

It maps the exe at its base (nothing is executed), and for all 6 load orders runs each dll's finders and hooks
on that copy in turn, then walks every patched function's stub chain. It must end in `ALL OK` for the game,
editor and server exe after any game update; a failing finder prints which pattern it lost.

## Status (Nov 1 2026)

- The split dlls are **built and offline-tested only**: `chain_test` prints `ALL OK` on all three 1.067 exes
  (6/6 load orders, SpawnBullet chained 3 deep, every chain ends in the original bytes). **Not yet run in game.**
- The feature code is the 0.5.1 code moved across unchanged, apart from: each dll's own ini section/log, log
  lines no longer carry an `overcharge:` prefix, lock-on has its own `AllowMultiplayer` key, overcharge needs only
  the owner offset from GetAimPoint (0.5.1 required the whole lock-on finder set before overcharge could start).
  Removed from overcharge: the pressed-button look (eCommand::Update / eItemButton::SetLook hooks, never gave a
  lasting state) and the `OverchargeRounds` setting with its magazine-capacity rule (always 1 round per shot now).
- In-game status of the features themselves (tested in the old combined dll): see each README.

## Not included

**Bomb bay airstrikes** (GEM Converge 0.6 work: Marauder bombers dropping their own bombs from the bay on
`{action "airstrike:…"}`; vtable patches on eAirstrike / eBombDropOrder + a GetVelocity hook). It is only in the
old combined `VCOMP140.dll`, so removing that dll removes it. It shares no hooks with these three and could become
a fourth dll the same way.
