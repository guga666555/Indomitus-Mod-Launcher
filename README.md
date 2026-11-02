# GEM Launcher — DLL injector launcher for Call to Arms: Gates of Hell

A windowless `GEM Launcher.exe` with its own icon that starts `call_to_arms.exe` and loads a DLL
of your choice into it before the game runs its own code. Built for GEM BreedTags / `gem_ext`
style engine extenders. Ships with a stub `gem_ext.dll` that only logs, to prove injection works.

## Build (on your Windows PC) — pick one

**A. zig (no Visual Studio)** — the toolchain GEM BreedTags already uses:
```
pip install ziglang
build_zig.bat
```
**B. MSVC** — Visual Studio Build Tools with *Desktop development with C++*:
```
build.bat
```
Either way the output is `dist\` (zig) or `build\dist\` (MSVC): `GEM Launcher.exe`,
`gem_ext.dll`, `gem_launcher.ini`. Everything is x64 and statically linked.

## Your own icon

```
python tools\make_icon.py path\to\your_art.png            (writes launcher\app.ico)
python tools\make_icon.py art.png --round --pad 0.06      (circle mask, small margin)
```
then rebuild. The script centre-crops to a square and writes every size Windows uses
(16–256 px, 256 as PNG). Square art ≥ 512 px looks best; keep the subject bold — Explorer
mostly shows it at 32–48 px. Version info (name, description, 1.0.0.0) is in
`launcher\launcher.rc` if you want to change what Properties shows.

A placeholder icon is included so the build works out of the box.

## The window

Double-clicking `GEM Launcher.exe` opens a small dark window with **Launch Game** and
**Launch Editor**. Buttons play `sounds\hover.wav` when the mouse enters them and
`sounds\click.wav` when pressed — swap those WAVs for any 16-bit PCM WAV you like, or point
`sound_hover=` / `sound_click=` in the ini at other files (empty = silent). The status line
shows what it found (game, editor, DLL) and what it is doing.

After a successful launch the window hides and the launcher waits for the game to exit (so the
log gets the exit code), then closes. Esc or the X closes the window.

Shortcuts that skip the window: `"GEM Launcher.exe" --game` and `"GEM Launcher.exe" --editor`.

## Run

1. Steam must be running.
2. `dist\` can live anywhere; it does not have to be inside the game folder.
3. Double-click `GEM Launcher.exe` and pick Game or Editor. It finds the game through Steam's
   library files (editor = `call_to_arms_ed.exe` next to it, or `editor_exe=` in the ini), starts
   it suspended, injects, resumes. Failures show a message box; `gem_launcher.log` is written
   next to the exe; `console=1` in the ini adds a live log window.
4. GEM BreedTags writes `gem_breedtags.log` **next to its own DLL**, so check
   `GEM_BreedTags_0.5\gem_breedtags.log` for the "loaded into call_to_arms.exe" line.

Command line overrides the ini: `"GEM Launcher.exe" --game --dll C:\x\gem_breedtags.dll --console -- -mod my_mod`
(anything after `--` is passed to the game). Desktop shortcuts keep the icon.

## Modes

| mode  | how                                                                 | when the DLL loads                       |
|-------|---------------------------------------------------------------------|------------------------------------------|
| spawn | `CreateProcess(CREATE_SUSPENDED)` → `QueueUserAPC(LoadLibraryW)` on the main thread → resume | during loader init, before the entry point |
| watch | opens `steam://rungameid/400750`, waits for the process, `CreateRemoteThread(LoadLibraryW)` | tens of ms after the game started |

Spawn sets `SteamAppId`/`SteamGameId` in the child environment, the same way Steam does, so
`SteamAPI_RestartAppIfNecessary` does not relaunch the game. If the DLL is not visible 5 s after
resume, spawn falls back to a remote thread by itself. Use watch only if spawn misbehaves.

## Notes for the injected DLL

- Early-bird means `DllMain` runs before the game's entry point, under the loader lock: static
  imports (kernel32, user32, steam_api64…) are resolved, nothing the game loads dynamically is.
  GEM BreedTags is fine with this: it patches the exe from `DllMain`/a watcher thread and reads
  its ini relative to its own path.
- Injecting a file literally named `dbghelp.dll` works (the real one is already loaded from
  System32 by then, so you get two same-named modules), but a copy renamed to
  `gem_breedtags.dll` is cleaner. Nothing inside it depends on the name.
- The editor and dedicated server are separate exes: `--exe ...\call_to_arms_ed.exe` to inject
  there too, or keep the dbghelp-proxy install for those.

## Caveats

- Offline / own-lobby use only. Every simulating process needs the same DLL or games desync, and
  any anti-cheat would flag injection.
- Some antivirus products dislike `WriteProcessMemory` + `QueueUserAPC`. If the exe is
  quarantined, exclude the `dist` folder.

## Auto-update (no server needed)

Release builds can update themselves from any static HTTPS host (GitHub Releases works for free).

1. In `gem_launcher.ini` set `update_url=` (where `update.ini` lives) and `update_exe_url=` (where `Indomitus.exe` lives).
   With GitHub: create a repository, and use
   `https://github.com/OWNER/REPO/releases/latest/download/update.ini` and `.../latest/download/Indomitus.exe`.
2. Raise `version=` and run `build_zig.bat`. It writes `dist\release\Indomitus.exe` and `dist\release\update.ini`.
3. Create a new GitHub release and attach both files. Done: launchers see it on their next start, ask, download,
   verify and restart themselves.

Optional `update_min_version=`: launchers older than that cannot start the game until they update (keeps every
multiplayer peer on the same extension). Offline players are never blocked.

**Signing key.** The first build creates `%USERPROFILE%\.indomitus\update_signing.key`. Back it up and never share
it. Every update.ini is signed with it, and launchers only install updates signed by that key, so a hijacked
download link cannot push someone else's exe. If you lose the key, installed launchers can no longer update
(players would need a new launcher by hand). Building on another PC: copy the key file there, or point the
`INDOMITUS_SIGN_KEY` environment variable at it.

The first launcher that has `update_url=` set must reach players by hand once; from then on it updates itself.

### Building your own copy (forks)

`launcher\update_pubkey.h` is the public key of the official Indomitus releases. If you build your own launcher,
delete that file and empty `update_url=` / `update_exe_url=` in `gem_launcher.ini` (or point them at your own
releases): the first build then creates your own signing key. Official launchers only accept updates signed with
the official key, so a fork can never update them.
