# Indomitus Launcher

Starts *Call to Arms – Gates of Hell* (or its map editor) with the Indomitus engine-extension DLLs
loaded into the process before the game runs its first line of code. Nothing in the game folder is
changed: start the game from Steam and it is untouched.

The DLLs are sealed into the exe. Players download one `Indomitus.exe`, put it anywhere, and run it.
Every player in a multiplayer match needs the same launcher build (the code is shown in the window).
In a lobby the game says so itself: the launcher passes its version and build code to the game
(`INDOMITUS_LAUNCHER_VERSION`, `INDOMITUS_PAYLOAD`), `gem_lobbycheck` publishes them in the Steam lobby
and shows a message box when the host and a player differ.

## What it does on start

1. Extracts the embedded DLLs and their inis to `%LocalAppData%\Indomitus Launcher\payload\<code>\`,
   verifying every file against its SHA-256. Each DLL writes its own log into that folder.
2. Checks GitHub for a newer launcher and offers it (see *Auto-update*).
3. **Play** / **Editor**: makes sure Steam is running and logged in (starts it if needed), switches the
   marked mods on in the game profile (see `mods_marker=`), adds the mod's ability hotkeys that are
   missing from the profile's `keys.set` (see `hotkeys=`), starts the exe suspended, loads the DLLs,
   verifies they are in, then hides until the game exits. If a DLL fails to load the game is closed
   instead of running half-patched.
4. While the game runs: when you host a multiplayer game the game starts `call_to_arms_server.exe`.
   `gem_hostserver` (in the game) makes it start frozen and tells the launcher, which writes a second
   verified copy of the payload to `...\payload\<code>\server\` and loads the same DLLs into the
   server before its first instruction; then the server is let go. Should that not happen, the launcher
   still finds the server in the process list and loads the DLLs into it while it runs (late: a race
   with the server's script loading). This needs the launcher to stay open, so it does not happen with
   `--no-wait`.
5. When the game exits: gathers that run's logs (launcher, `game.log`, every DLL log, the host server's
   logs, a list of new crash dumps) into one file,
   `%LocalAppData%\Indomitus Launcher\logs\session_<date>_<time>_<exe>.log` (the newest 20 are kept).
   It is the file to ask players for; it contains `game.log`, so paths with the Windows user name.

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
`version.rc`, `settings.gen.ini`. `launcher\update_pubkey.h` is generated once and committed (public key only).

### The extension DLLs

Each DLL is built on its own and its `.dll` + `.ini` are committed in `launcher\payload\`, so the
launcher builds without rebuilding them. To rebuild one, run `build.bat payload` in its folder (it
compiles, then copies the two files into `launcher\payload\`), then `build_zig.bat`. The build
scripts use the `zig\` folder that `build_zig.bat` downloads. (Fauna's `build.bat` has no `payload`
step yet: copy `gem_fauna.dll` and `gem_fauna.ini` from its `dist\` folder by hand.)

| Payload DLL | Folder under `_dlls\` | What it adds |
|---|---|---|
| `gem_boardfilter` | `_indomitus_DLL_boarder` | who may crew / board a vehicle, by `board_` / `boarder_` tags |
| `gem_bombbay` | `_indomitus_DLL_new_airstrike` | bomb-bay airstrikes |
| `gem_breedtags` | `_indomitus_DLL_BreedTags_0.5` | breed tags become unit tags; `breed_applied` event |
| `gem_conquestui` | `_indomitus_DLL_conquest_UI_changes` | dynamic campaign: unit preview, research tree zoom |
| `gem_contactdamage` | `_indomitus_DLL_contactdamage` | melee contact damage to a vehicle's body |
| `gem_costred` | `_indomitus_DLL_costred\src` | MP buy menu: cost in red when you cannot afford the unit |
| `gem_crewvoice` | `_indomitus_DLL_crewvoice\src` | voice lines for vehicles without a crew |
| `gem_fauna` | `_indomitus_DLL_fauna` | creature units behave like infantry squads |
| `gem_hostserver` | `_indomitus_DLL_hostserver` | multiplayer: the host server starts frozen until the launcher has loaded the DLLs into it |
| `gem_itempen` | `_indomitus_DLL_itempierce` | rounds that pierce a worn item carry on into the wearer |
| `gem_lobbycheck` | `_indomitus_DLL_lobbycheck` | multiplayer: a message when host and players run different launcher builds |
| `gem_meleechase` | `_indomitus_DLL_meleechase` | fauna and vehicles chase melee targets |
| `gem_meleestance` | `_indomitus_DLL_meleestance` | melee stance, charge and auto-draw |
| `gem_noprone` | `_indomitus_DLL_gem_noprone\source` | `cannot_lie` units never go prone by AI decision |
| `gem_repairfilter` | `_indomitus_DLL_repairfilter` | who may repair a vehicle, by `repair_by_` tags |
| `gem_researchpath` | `_indomitus_DLL_research_path` | research tree: path highlight and "Requires" tooltip |
| `gem_revivefilter` | `_indomitus_DLL_revivefilter` | who may revive a downed soldier, by `revive_by_` tags |
| `gem_scriptext` | `_indomitus_DLL_scriptext` | script conditions, damage options and events |
| `gem_sensortags` | `_indomitus_DLL_sensortags` | `sensor_off` / `no_target` tags |
| `gem_sessiondump` | `_indomitus_DLL_sessiondump` | diagnostic: a minidump at every exit (shipped switched off) |
| `gem_squadfollow` | `_indomitus_DLL_squadfollow` | squad members follow without restarting their run |
| `gem_teamsound` | `_indomitus_DLL_teamsound\src` | per-player sound variants (`@side`, `@army`, `@who`) |
| `gem_teamtags` | `_indomitus_DLL_teamtags` | automatic `team<N>` tags by owning player |
| `gem_teleport` | `_indomitus_DLL_teleport` | the `{teleport}` action (jetpacks) |
| `gem_unitname` | `_indomitus_DLL_unitname` | personal names for units that are not soldiers, from `name_` def props |
| `gem_usercontrol` | `_indomitus_DLL_usercontrol\src` | `user_control` tagged units go to their player |
| `gem_weaponscript` | `_indomitus_DLL_new_weaponscript` | convergence, overcharge, guided projectiles |

## Versioning

`version=` in `gem_launcher.ini` is the one and only version. It appears in the window, the title bar,
the exe's file properties and `update.ini`; use it as the GitHub release tag (`v0.9.4`). The build
warns when it is not higher than the newest git tag.

Never edit `update.ini` by hand: it is signed when it is built, and any change (even the version
number) makes every launcher reject it.

## Auto-update (no server needed)

Launchers fetch `update.ini` from the newest GitHub release and offer a newer version. The download
is checked against the SHA-256 and the ECDSA signature in that manifest; only manifests signed with
the project key are accepted, so a hijacked download link cannot push someone else's exe.

Releasing a new version:

1. Commit and push your changes (GitHub Desktop: Commit, then Push origin).
2. Raise `version=` in `gem_launcher.ini` and **save** it.
3. Run `build_zig.bat`. Check that it prints the version you set.
4. On GitHub: **Draft a new release**, create the tag `v<version>` there, attach `Indomitus.exe` and
   `update.ini` from `dist\release\`, leave "pre-release" unticked, and **Publish release**.
   Launchers see it on their next start.

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
| `mods_marker=` | words that mark a mod for the launcher (default `INDOMITUS MOD LAUNCHER`; case, brackets, underscores and hyphens are ignored, so `[INDOMITUS_MOD_LAUNCHER]` matches too): every local or Workshop mod with it in its `mod.info` `{desc "..."}` is switched on before launch, alphabetically by folder name |
| `mods_exclusive=` | 1 = switch every other mod off |
| `hotkeys=` | default keys for the mod's ability buttons, added to the profile's `keys.set` when the slot is missing there (`name=VALUE;name=VALUE`, a slot that exists is never changed; empty = off) |
| `mods_profile=` | profile id to edit; empty = the most recently saved profile, searched in both `Documents\my games\gates of hell\profiles` and `%LocalAppData%\digitalmindsoft\gates of hell\profiles` (the game uses either, depending on the PC) |
| `show_mod_folder_button=`, `show_disable_mods_button=` | hide the two developer buttons for end users |
| `show_features_button=` | 0 = hide the "Show the engine features" button |
| `require_steam=`, `steam_autostart=`, `steam_timeout_ms=` | the Steam check before launching |
| `update_url=`, `update_exe_url=`, `update_min_version=` | auto-update |
| `build_label=`, `subtitle=`, `title_font=`, `title_size=` | window text |
| `exe=`, `editor_exe=`, `appid=`, `args=` | game location (empty = found through Steam) and extra game arguments |
| `sound_hover=`, `sound_click=` | `embedded`, `none`, or a .wav path |
| `wait_for_exit=` | 1 (default) = the launcher stays open, hidden, until the game exits: needed for the session log and for the host server |
| `console=` | 1 = open a live log window |

## Icon and logo

`python tools\make_icon.py art.png` writes `launcher\icon.ico` (every size Windows uses);
`python tools\make_logo.py logo.png` writes `launcher\res\logo.bmp` (32-bit with alpha). Rebuild after.
