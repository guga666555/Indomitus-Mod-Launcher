# GEM UnitName 0.1

Personal names for units that are not humans (walkers, daemon engines, any vehicle).

## What it does

Soldiers get a name from the localization lists `names/<nationality>/first` and `names/<nationality>/last`.
Vehicles have no name of their own: the name line of the unit panel shows the def's `info/special/<def>` text or
the vehicle class.

With this DLL, a non-human unit whose def has a prop `name_<list>` shows a name built from
`names/<list>/first` (titles) and `names/<list>/last` (names) in that line instead, in the same lists format as
soldier names (one name per line).

- The pick comes from the unit's entity id (the `0x....` id in map and save files): the same unit keeps its
  name on every refresh, after save / load, and every MP player sees the same name.
- Humans are never touched.
- No `names/<list>/last` text: the unit keeps the normal line. No `first` text: the name is the last name alone.

## How to give a unit a name

1. Def: add `"name_<list>"` to the props line, e.g. `"name_sm_dreadnought"`.
2. Localization (`localizations\<lang>\interface\text\names\*.po(t)`):
   `msgctxt "names/<list>/first"` and `msgctxt "names/<list>/last"`, names separated by `\n`.

Dreadnoughts: `name_sm_dreadnought` (Brother ...), venerable ones `name_sm_dreadnought_venerable`; lists in
`dlg_names_sm.pot` / ru / ua `.po` (written by `add_dread_names.py`).
Ogryns `name_ogryn`, chaos Ogryn `name_ogryn_chaos` (`dlg_names_ig`), Bloodletters / Bloodcrushers
`name_khorne_daemon` (`dlg_names_tg`): last names only, written by `add_names.py` (which also tops up the SM soldier
lists and adds lists for SM nationalities that had none).

## Settings (`gem_unitname.ini`)

| Key | Default | Meaning |
|---|---|---|
| Enabled | 1 | 0 = do nothing |
| Prefix | name_ | start of the def prop that names the list |
| Format | `<c(c3baae)>{first} {last}</c>` | text shown; vanilla uses that colour for this line |
| Log | 0 | 1 = log the first 50 names and any missing list |

Log: `gem_unitname.log` (editor `gem_unitname_editor.log`). The first line says whether it installed.

## Multiplayer

UI only. The server exe is left alone. A player without the DLL sees the normal line.

## Build

`build.bat` (zig from `..\..\zig`, PATH or `python -m ziglang`); `build.bat payload` copies the dll + ini into `launcher\payload`. `build.bat test` runs the finders and the patch on
the 1.068 game and editor exes (one exe per process) and the name logic on fake objects.

Engine notes: `_notes\dll_hooks.md`, section "Unit names for non-humans".
