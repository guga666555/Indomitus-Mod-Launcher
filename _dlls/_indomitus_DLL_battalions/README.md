# GEM Battalions

Payload DLL for the Indomitus Launcher (Call to Arms: Gates of Hell 1.067, x64).

Goal: Men of War II-style **battalions** in Conquest. Instead of one army, several battalions with
their own rosters sit on nodes of the strategic map, move between nodes, and fight the battles on
their node; a node attacked with no battalion on it is lost without a fight (for the AI too).
Design and phases: project doc `gem-battalions-feasibility.md`.

## 0.1.0 - phase-0 probe (this version)

**Logs only. Changes nothing in the game.** It maps the campaign logic that later phases hook:
the campaign state object and roster, regions and ownership, turn end, enemy attacks, battle
launch and result write-back, save/load.

No engine address is hard-coded. At startup the DLL reads the exe's MSVC RTTI (type descriptors,
complete object locators, vtables) and works from class names:

| What | How | Log tag |
|---|---|---|
| Class list | every class matching `[trace] classes` with its vtable RVA and slot count | startup |
| Call trace | each virtual slot of those classes is wrapped by a transparent trampoline (saves rcx/rdx/r8/r9/xmm0-3, logs, tail-jumps to the original; arity-agnostic) | `[trace] Class::vt[i] #n this= a2= a3= a4= from exe+...` |
| Campaign view | `eGUI_DynamicCampaignView` vt[69] Bind: field dump (strings, vectors, object pointers with class names), recursing into `[dump] classes`; first vt[20] Update: child dialog tree + `.data` scan for campaign singletons; vt[34] Notify: arguments | `[view]`, `[dump]`, `[global]` |
| State diffs | tracked objects are snapshotted; on Notify, Update, scene loads and save-file opens the changed qwords are logged | `[diff]` |
| Scene loads | `Scene::eManager` vt[5] Load: scene name, descriptor strings, caller | `[scene]` |
| Files | `CreateFileW` (exe IAT) on profile / campaign / save paths, with caller | `[file]` |
| Summaries | call counts per slot at every scene load, view destruction and exit | `[summary]` |

Hooks are installed from a worker thread `install_delay_ms` after injection, so this DLL wraps
the slots *after* the other payload DLLs (ConquestUI hooks the same view slots in DllMain); slots
already pointing outside the exe are logged as `already hooked by <module>`.

In the editor and the dedicated server the DLL writes one line and stays inert (`game_only=1`).

### What to do in game
1. Build, copy `dist\gem_battalions.dll` + `gem_battalions.ini` into `launcher\payload\`,
   rebuild the launcher, start the game through it.
2. Open an existing Conquest save (or start one). Buy a unit. Open and close the research tree.
3. Start a battle, finish it (win or lose), return to the map. Let the enemy take its turn,
   including one enemy attack on your territory.
4. Save the campaign, go back to the main menu, load it again.
5. Send `gem_battalions.log` from `%LocalAppData%\Indomitus Launcher\payload\<fingerprint>\`.

Each step is bracketed in the log by `[view] Notify` / `[scene] Load` / `[file]` lines with diffs
of the tracked objects in between, which is what phase 1 needs.

### If the log is too big or too quiet
* Too big: raise `[trace] every`, add noisy classes to `exclude`, or lower `max_log_mb`.
* Too quiet (campaign state classes have other names): look at the startup class list, then widen
  `[trace] classes`, e.g. `*Campaign*;*Conquest*;*Region*;*Army*;*Turn*`.
* `eGUI_DynamicCampaignView: NOT FOUND` means the RTTI scan failed; send the log as it is.

## Build
`build.bat` - needs the launcher's portable zig (`..\..\zig\zig.exe` when this folder is in
`_indomitus_mod_launcher\_dlls\`), or zig on PATH, or `python -m ziglang`. Output in `dist\`.

Host checks (no Windows needed):
* `cc -I src tests/logic_test.c -o logic_test && ./logic_test` - demangler and pattern lists.
* `clang --target=x86_64-windows-gnu -std=c11 -O2 -Wall -Wextra -I tests/stub -c src/gem_battalions.c`
  - compiles the DLL source against fake Win32 headers (on Linux add
  `-isystem /usr/include -isystem /usr/include/x86_64-linux-gnu`).

## Files
* `src/gem_battalions.c` - the DLL
* `src/gb_names.h` - RTTI name demangling and pattern lists (shared with the test)
* `gem_battalions.ini` - settings, documented inline
* `tests/` - host-side test and stub headers
