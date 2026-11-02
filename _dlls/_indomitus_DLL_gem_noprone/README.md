# GEM NoProne 0.5

Stops the **AI** from going prone for units whose breed (or the unit itself) has a tag (default `cannot_lie`).
Player stance orders are untouched. Game 1.067.0: game, editor and dedicated server
(`call_to_arms.exe`, `call_to_arms_ed.exe`, `call_to_arms_server.exe`). Loaded only by the Indomitus Launcher.

## Install
1. `source\build.bat payload` - builds `source\out\gem_noprone.dll` and copies it with `gem_noprone.ini`
   into `launcher\payload\`.
2. Run `build_zig.bat` in the launcher folder.
3. Start through the launcher. `%LocalAppData%\Indomitus Launcher\payload\<code>\gem_noprone.log` must end with
   `AI prone block ON` and `simulation change: every player of a multiplayer match needs this DLL`.

## Use
Add the tag to a breed (needs a restart; breeds are read at load):

```
{tags "cannot_lie"}
```

or give it to a unit as an entity tag: `{tags add "cannot_lie"}` in a script, or the unit's tags in the
editor. Entity tags take effect immediately. With GEM BreedTags in the payload, breed tags are also copied to
the unit's entity tags, so either path matches.

The log says `prone blocked for breed "..." (breed|entity tag)` the first time each breed is stopped.

## How it works
The engine already has a `cannot_lie` stuff tag (vanilla `rpg_weapon.pattern`): the AI's
`CanTakePose(brain, pose)` (eHumanBrain.cpp) returns false for pose 1 (lie) when the held item carries it.
The hook adds one more case: the breed's `{tags}` or the unit's entity tags contain the ini `Tag`.
Every caller of `CanTakePose` is AI brain code (enemy reaction, injured crawl, return to zone).

All addresses are found at runtime. Function starts come from the exe's `.pdata` table, not from the first
bytes of the function, because other payload DLLs patch those: BreedTags puts a 14-byte jmp over ApplyBreed,
which is what switched 0.4 off in every launcher run (`ApplyBreed: prologue not found`). If CanTakePose itself
is ever hooked by another DLL first, NoProne refuses to hook over it and says so in the log.

The hook is a 15-byte prologue copy into a trampoline plus a 14-byte `jmp [rip]` + nop over the original,
written with every other thread suspended (same routine as BreedTags).

## Status
- 0.5 finders pass offline on game `6AB3DCC7`, editor `6AB3DD83` and server `6AB3DD21`, both on the clean exe and
  with a BreedTags-style hook written over ApplyBreed (`tests/run_test.sh`). Not yet run in the game.
- 0.4 was OFF in every launcher run 27-30 Oct (ApplyBreed finder). 0.2 (proxy build) was tested in the editor with
  an entity tag and blocked prone as intended.
- Not covered: grenade dodge (`grenade-lie`), blast knockdown / throw-off.
- Simulation change: in multiplayer every player must run the same payload (the launcher guarantees it).
- Open: the 0.2 editor run had 651 `!animator->IsWorking()` assertions (eHumanFsm.cpp:420) on the bolter marines,
  with no "off" run to compare; check once 0.5 is ON.

## Engine facts (1.067; the finders re-derive them, values are for reference)

| What | game | editor | server |
|---|---|---|---|
| `CanTakePose(brain, pose)` | exe+0x942FC0 | exe+0x7CA5F0 | exe+0x757FA0 |
| `ApplyBreed` | exe+0x95CAD0 | exe+0x7E4120 | exe+0x771AB0 |
| brain -> human | +0x38 | +0x38 | +0x38 |
| human -> breed | +0x1390 | +0x1390 | +0x1388 |
| breed `{tags}` container | +0x128 | +0x130 | +0x128 |
| entity `{tags}` container | +0x1D0 | +0x1D0 | +0x1D0 |

- Pose 0 = stand, 1 = lie, 2 = squat. Returning 0 for pose 1 = "can't lie".
- Entity byte +0x30 bit 2 (0x04) = human; the engine's own sibling brain function tests it before using +0x1390.
- TagContainer: +0x308/+0x310 point to itself (sanity check), +0x318/+0x320 = vector begin/end of 0x60-byte
  tags, MSVC `std::string` name at tag+0x40 (SSO 16, size +0x50, cap +0x58). Names are stored lowercase.

## Build / post-update check
`source\build.bat` uses the launcher's portable zig (`_indomitus_mod_launcher\zig\zig.exe`), else zig on PATH,
else `python -m ziglang`.
- `build.bat` / `build.bat payload` - the DLL
- `build.bat test` - `out\np_test.exe`; `np_test.exe ...\call_to_arms.exe` must end with `ALL FOUND`
  (runs the finders clean, with ApplyBreed hooked, and checks a hooked CanTakePose is refused)
- Linux/WSL: `tests/run_test.sh <exe> ...` does the same with the system C compiler.
