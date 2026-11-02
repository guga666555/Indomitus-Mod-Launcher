# Indomitus Launcher

Starts *Call to Arms – Gates of Hell* (or its map editor) with the Indomitus engine-extension DLLs
loaded into the process before the game runs its first line of code. Nothing in the game folder is
changed: start the game from Steam and it is untouched.

The DLLs are sealed into the exe. Players download one `Indomitus.exe`, put it anywhere, and run it.
Every player in a multiplayer match needs the same launcher build (the code is shown in the window).

## What it does on start

1. Extracts the embedded DLLs and their inis to `%LocalAppData%\Indomitus Launcher\payload\<code>\`,
   verifying every file against its SHA-256. Each DLL writes its own log into that folder.
2. Checks GitHub for a newer launcher and offers it (see *Auto-update*).
3. **Play** / **Editor**: makes sure Steam is running and logged in (starts it if needed), switches the
   Indomitus mods on in the game profile, starts the exe suspended, loads the DLLs, verifies they are
   in, then hides until the game exits. If a DLL fails to load the game is closed instead of running
   half-patched.

`gem_launcher.log` next to the exe says what happened. Esc or the X closes the window;
`Indomitus.exe --game` / `--editor` skip the window.

## Build

Windows, no Visual Studio needed:

```
build_zig.bat          release build  -> dist\Indomitus Launcher.exe + dist\release\
build_zig.bat dev      DEV build: also loads dlls\*.dll next to the exe, reads a gem_launcher.ini next to it
```

The first run downloads the portable zig compiler (about 80 MB) into `zig\`. Everything the build
needs is in this repository:

| Path | What |
|---|---|
| `launcher\main.cpp` | the launcher (Win32, one file) |
| `launcher\payload\` | the DLLs and inis that get sealed in; every `.dll` here is injected, alphabetically |
| `launcher\lang\*.ini` | UI text, one file per language (`key=text`); English is the fallback |
| `launcher\res\` | background, logo, title font, button sounds |
| `gem_launcher.ini` | settings compiled into the exe (mods to enable, update URLs, …) |
| `tools\update_sign.c` | release signing / version stamping, built and run by `build_zig.bat` |
| `_dlls\` | source of each extension DLL, each with its own README |

Generated on every build and not committed: `launcher\payload.rc`, `payload.manifest`, `fonts.rc`,
`version.rc`, `settings.gen.ini`, `update_pubkey.h` is committed (public key only).

## Versioning

The version comes from the newest git tag named like `v0.9.4`. It appears in the window, the title
bar, the exe's file properties and `update.ini`. `version=` in `gem_launcher.ini` is only used while
the repository has no such tag. Tag only real releases, and only on `main`: the build takes the
highest tag it finds, whatever branch it is on.

## Auto-update (no server needed)

Launchers fetch `update.ini` from the newest GitHub release and offer a newer version. The download
is checked against the SHA-256 and the ECDSA signature in that manifest; only manifests signed with
the project key are accepted, so a hijacked download link cannot push someone else's exe.

Release steps:

1. Commit. In GitHub Desktop: History → right-click the commit → **Create Tag…** (`vX.Y.Z`) → Push origin.
2. `build_zig.bat`. It prints the version and writes `dist\release\Indomitus.exe` + `update.ini`.
3. On GitHub, draft a release from that tag, attach both files, publish it as the latest release
   (not a pre-release). Launchers see it on their next start.

Optional `update_min_version=` in `gem_launcher.ini`: launchers older than that cannot start the game
until they update (keeps every multiplayer peer on the same extension). Offline players are never
blocked.

**Signing key.** The first build creates `%USERPROFILE%\.indomitus\update_signing.key`. Back it up
and never share it or commit it. Lose it and launchers already installed can never update again.
Building on another PC: copy the key there, or point `INDOMITUS_SIGN_KEY` at it. The build refuses to
create a second key while `launcher\update_pubkey.h` exists.

### Building your own copy (forks)

`launcher\update_pubkey.h` is the public key of the official Indomitus releases. For your own
launcher, delete that file and set your own `update_url=` / `update_exe_url=` (or leave them empty
to disable updates): the first build then creates your own key. Official launchers only accept
updates signed with the official key, so a fork can never update them.

## Settings (`gem_launcher.ini`)

| Key | Meaning |
|---|---|
| `require_mods=` | mod folder names switched on in the profile before every launch (comma-separated) |
| `mods_exclusive=` | 1 = switch every other mod off |
| `show_mod_folder_button=`, `show_disable_mods_button=` | hide the two developer buttons for end users |
| `require_steam=`, `steam_autostart=`, `steam_timeout_ms=` | the Steam check before launching |
| `update_url=`, `update_exe_url=`, `update_min_version=` | auto-update |
| `build_label=`, `subtitle=`, `title_font=`, `title_size=` | window text |
| `exe=`, `editor_exe=`, `appid=`, `args=` | game location (empty = found through Steam) and extra game arguments |
| `sound_hover=`, `sound_click=` | `embedded`, `none`, or a .wav path |
| `console=` | 1 = open a live log window |

## Icon and logo

`python tools\make_icon.py art.png` writes `launcher\icon.ico` (every size Windows uses);
`python tools\make_logo.py logo.png` writes `launcher\res\logo.bmp` (32-bit with alpha). Rebuild after.
