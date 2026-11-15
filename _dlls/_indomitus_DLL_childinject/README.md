# GEM ChildInject 0.1

Launcher payload DLL. When you host a multiplayer game, `call_to_arms.exe` starts `call_to_arms_server.exe`. The launcher only injects into the game, so that server ran with no payload DLLs and crashed at script load on ScriptExt words (`{if health}` → "unknown condition", eIA_If.cpp:1200).

What it does:
- **In the game** it hooks `kernelbase!CreateProcessInternalW`, which every `CreateProcess*` and `ShellExecute*` call ends in. The hook moves the 17-byte prologue, with no relative operands, to a trampoline.
- **When the image is `call_to_arms_server.exe`:**
  - It adds `CREATE_SUSPENDED` to the start flags.
  - It copies every `*.dll` / `*.ini` in its own payload folder to `<payload>\server\`. The server's DLLs read the same inis but write their own logs. A file still locked by an earlier server is fine if it is identical; otherwise a fresh `server_<tick>` folder is used.
  - It queues `LoadLibraryW` APCs for those DLLs in alphabetical order, the same early-bird injection the launcher uses.
  - It resumes the server, unless the caller asked for it suspended.
- **Afterwards**, a worker thread checks the server's module list (15 s) and logs every DLL as verified or `NOT load`, then logs the server's exit code.
- **Inside the server** it only logs that it is loaded; it installs no hook there.
- **Log:** `gem_aa_childinject.log` in the payload folder (game side) and in `server\` (server side), appended, with the pid on every line.

It is named `gem_aa_…` so it sorts first in the payload.

Build: `build.bat` / `build.bat payload` / `build.bat test`, using the same zig flags as every payload DLL.

Tested Nov 7 2026:
- **Fake game + fake server:** CreateProcessW, ShellExecuteExW and caller-suspended starts all injected; an unrelated child (`cmd.exe`) was untouched.
- **Real 1.067 `call_to_arms_server.exe` + full payload:** all 14 DLLs loaded and every DLL logged its server finders. The server got past the old `{if health}` crash point. That run had no game owner and was killed after 10 s.
- **Real MP host through the local launcher build:** works (Alex, Nov 7 2026).
