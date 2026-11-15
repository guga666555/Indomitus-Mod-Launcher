# GEM HostServer 0.1

Indomitus Launcher payload DLL (`gem_hostserver.dll` + `gem_hostserver.ini`), game 1.067.
Needs the launcher from the same commit (`INDOMITUS_SERVER_SYNC`, "MP host server" in `launcher\main.cpp`).

Makes the multiplayer host server start frozen, so that the launcher can load the payload into it
before its first instruction.

**Status: written from the exe and from two crash dumps, tested offline only. It has not run in the
game yet.** `build.bat test` runs the real handshake on Windows against a stand-in launcher.

## Why

Hosting makes the game start `call_to_arms_server.exe`, which runs the match. A few seconds after its
start the server reads the mod's scripts and stops with `unknown condition (eIA_If.cpp:1200)` when
`gem_scriptext` is not in yet.

The game starts the server running and outside every job (`CreateProcessW` with
`CREATE_BREAKAWAY_FROM_JOB` at exe+0x3BB565, then its own kill-on-close job), so the launcher can only
find it in the process list once it runs, and then races the script loader:

| Launcher | Host server |
|---|---|
| 0.9.12 | never gets the DLLs (waited for a job notice that cannot come) |
| 0.9.13 | gets them late, from the process list. Fine on one PC (3 runs), lost by 76 ms on another: server created 15:21:31.1, script error 15:21:39.349, `{if health}` registered 15:21:39.425 |
| with this DLL | gets them before it runs |

## How

In `call_to_arms.exe` only (inert in the server and the editor):

1. The exe's import slot of `KERNEL32!CreateProcessW` (exe+0xFED2A8 in 1.067, found by name) is pointed
   at the DLL's function. One pointer; no code of the game or of Windows is patched.
2. A call whose program is `call_to_arms_server.exe` gets `CREATE_SUSPENDED` added. Every other call
   goes through untouched.
3. The DLL writes the new process id and thread id into a 32-byte shared block, signals the launcher
   and waits for its answer (at most `WaitSeconds`, or until the launcher process ends).
4. The launcher checks that the process is `<game folder>\call_to_arms_server.exe`, writes its own
   SHA-256 checked copy of the payload to `<payload>\server\` and queues the same `LoadLibraryW` calls
   on the frozen first thread as it does for the game. Then it answers.
5. The DLL resumes the server. The DLLs load in payload order before the server's entry point.

The two sides meet through `INDOMITUS_SERVER_SYNC`, a name the launcher makes up for every game start
(16 random hex digits + its pid) and passes in the environment: events `Local\<name>-req` / `-ack`
and the block `Local\<name>-map`.

If the launcher is not listening (closed, `--no-wait`, no answer in time) the server is started as the
game asked, and the launcher's process-list watch is the backup. That backup now loads `gem_scriptext`
and `gem_teleport` first and no longer holds a server thread per DLL.

### Compared with the removed ChildInject

ChildInject copied the payload folder into `server\` and injected whatever it found there, unverified,
and patched `CreateProcessInternalW` inline. This DLL copies nothing and loads nothing: the launcher
still does both, from its own checked resources, and the hook is an import-slot pointer.

## What to look for

`gem_hostserver.log` (payload folder):
```
ON: import slot exe+0xFED2A8 (KERNEL32.dll!CreateProcessW) -> this dll
host server pid N created frozen (thread T) - waiting for the launcher to load the payload
launcher answered after 900 ms: payload queued - server resumed, the dlls load before its first instruction
```
`gem_launcher.log`:
```
host server: watching pid G for ...\call_to_arms_server.exe - gem_hostserver.dll hands it over frozen; ...
host server: the game starts pid N frozen (thread T) and waits for the payload
host server: all 24 dll(s) verified in pid N (loaded before its first instruction)
```
`found in the process list, already running` or `loaded late` in the launcher log means the backup ran:
the DLL was not in the payload, switched off, or its wait ran out.

## Build

`build.bat` -> `dist\gem_hostserver.dll` + `.ini`; copy both into `launcher\payload\`, then
`build_zig.bat` (or `build.bat payload`, which copies them). `build.bat test` -> offline test
(`test\hs_test.inc`, compiled into `src\gem_hostserver.c` with `-DHS_TEST`).

## Settings (`gem_hostserver.ini`, sealed into the launcher)

| Key | Default | |
|---|---|---|
| `Enabled` | 1 | 0 = hooks nothing (the launcher falls back to the process list) |
| `WaitSeconds` | 30 | how long a frozen server waits for the launcher before it is started anyway (2..300) |
| `Log` | 1 | `gem_hostserver.log`, rewritten every run |
