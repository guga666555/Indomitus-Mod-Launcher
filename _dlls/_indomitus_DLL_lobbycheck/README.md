# GEM LobbyCheck 0.1

Indomitus Launcher payload DLL (`gem_lobbycheck.dll` + `gem_lobbycheck.ini`), game 1.067.

Warns when the players in a multiplayer lobby do not all run the same launcher build. It compares
and tells; it never blocks a match and it changes nothing in the simulation.

**Status: built from reading the exe and steam_api64.dll, tested offline only. It has not run in the
game yet.** See *First test* below.

## What the players see

A Windows message box on top of the game (the game keeps running behind it):

- **A player who joins** a lobby whose host has another build: both versions and build codes, and who
  has to update ("Your launcher is the older one ..." / "The host's launcher is the older one ...").
  The same box, worded differently, when the host reports no build within `GraceSeconds` (a launcher
  from before this DLL, or a game started without the launcher).
- **The host**, for every player in his lobby whose build differs or who reports none, with the
  player's Steam name (`HostNotice=0` turns this off).

The same finding about the same player is not shown again within 60 seconds. In exclusive fullscreen
Windows may minimise the game when the box appears; click the game in the taskbar to return.

"Build code" is the launcher's payload fingerprint (the 8 characters in the launcher window and on
the features page). It is what decides: two launchers with the same version number but different
engine files count as different.

## How it works

1. The launcher starts the game with `INDOMITUS_PAYLOAD` (build code) and
   `INDOMITUS_LAUNCHER_VERSION` in the environment (`BuildEnvironment` in `launcher\main.cpp`).
   Without `INDOMITUS_PAYLOAD` the DLL hooks nothing.
2. The game keeps every multiplayer session in a Steam lobby (`eSteamLobby`, `eSessionLobby`,
   `eSessionLobbyClient` / `eSessionLobbyServer` in the exe; it calls `CreateLobby`, `JoinLobby`,
   `SetLobbyData` x30, `SetLobbyMemberData` x9 on `SteamMatchMaking009`).
3. The DLL points the exe's import slot of `steam_api64!SteamAPI_RunCallbacks` at its own function.
   On that thread, once Steam is up, it registers four ordinary Steam callback listeners
   (`LobbyEnter_t` 504, `LobbyDataUpdate_t` 505, `LobbyChatUpdate_t` 506, `LobbyCreated_t` 513) and
   does all its work after Steam's own dispatch. No game code is patched and nothing is searched by
   byte pattern, so it does not depend on the game build as long as the exe imports that function.
4. Through the flat C exports of `steam_api64.dll`:
   - lobby owner: `SetLobbyData` keys `indomitus_payload`, `indomitus_launcher`
   - everyone: `SetLobbyMemberData` with the same two keys
   - not the owner: `GetLobbyData` of the owner's two keys, compare
   - owner: `GetLobbyMemberData` of every member, compare
   The game reads its own lobby keys by name (`gamever`, `hostname`, `mapname`, `mods`, ... 33 keys;
   member keys `teamid`, `army`, ...) and never lists them, so the two extra keys are invisible to it.
5. The message box runs on its own thread.

Active in `call_to_arms.exe` and in `call_to_arms_server.exe` (the MP host server): whichever of the
two holds the lobby. A named mutex per lobby and player keeps them from showing the same box twice.
Inert in the editor.

Values from other players are untrusted: cut to 39 characters, only `[0-9A-Za-z._+-]` kept, never used
as a format string.

## What was checked, and how

| Checked | How |
|---|---|
| vtable order of a Steam callback object: `Run(data, ioFailure, call)`, `Run(data)`, `GetCallbackSizeBytes` | the game's own listeners (`CCallbackInternal_OnLobbyEnter` ... in `call_to_arms.exe`): slot 0 jumps to slot 1, slot 2 returns the size |
| callback struct sizes 24 / 24 / 32 / 16 | the same size functions in the exe |
| `SteamAPI_RegisterCallback` writes the flags byte at +8 and the id at +12, needs no Steam init, takes no lock | its code in `steam_api64.dll` (hence: registered on the callback thread) |
| the 14 flat functions and their arguments | their code in `steam_api64.dll` (thin jumps into the interface vtable; the id-returning ones return a plain 64-bit value) |
| every call of `SteamAPI_RunCallbacks` goes through the import slot | references in all three exes |
| the lobby logic | `build.bat test`: 12 scenarios on a fake Steam with several "machines" running the DLL's core |
| export and import lookups | `build.bat test` on the real `steam_api64.dll`, `call_to_arms.exe`, `call_to_arms_server.exe` |

Not checked, because it needs the running game: that the callbacks arrive as expected, which of the
two processes holds the lobby on the host, how the message box behaves over the game window.

## First test

One PC is enough for the first half:

1. Build (`build.bat`), copy `dist\gem_lobbycheck.dll` and `.ini` into `launcher\payload\`, run
   `build_zig.bat`, start the game with the new launcher.
2. Host a multiplayer game and wait in the lobby for a few seconds, then quit.
3. `%LocalAppData%\Indomitus Launcher\payload\<code>\gem_lobbycheck.log` should read:
   `ON: import slot ...`, `Steam is up: listening ...`, `lobby N: in it (created)`,
   `lobby N: I am the host - published version X, build Y`. If the last two lines are in
   `server\gem_lobbycheck_server.log` instead, the host server holds the lobby: also fine.

Two PCs, same launcher: both logs end with `... has the same build`, no box.
Two PCs, different launchers (keep an older `Indomitus.exe` for this): a box on both.

## Build

`build.bat` (zig, like the other payload DLLs) -> `dist\`. `build.bat payload` also copies the two
files into `launcher\payload\`. `build.bat test` builds and runs the offline test
(`test\lc_test.inc`, compiled into `src\gem_lobbycheck.c` with `-DLC_TEST`).

## Settings (`gem_lobbycheck.ini`, sealed into the launcher)

| Key | Default | |
|---|---|---|
| `Enabled` | 1 | 0 = hooks nothing |
| `HostNotice` | 1 | 0 = the host is not told about players (it still publishes its build) |
| `GraceSeconds` | 10 | time a host / player has to report before it counts as "reports none" (2..120) |
| `ShowMessage` | 1 | 0 = log only |
| `Log` | 1 | `gem_lobbycheck.log` / `gem_lobbycheck_server.log`, rewritten every run |
