# GEM CrewVoice 0.1

Engine voice lines for vehicles that have no crew (walkers, drones, daemon engines).

## What it does

A vehicle speaks with the nationality of its first crew member. A crewless unit has nobody to ask, so the
engine finds no language and stays silent, even though it raises every talk moment on the unit (select, move
order, attack, kill...) and runs its idle / in-battle timer.

With this DLL, when the engine finds no language for a vehicle and the vehicle's def has a prop that starts
with `voice_`, that prop is used as the language.

- A unit that has a crew keeps the crew's language. The prop is only a fallback.
- Humans are never touched.
- Script `{talk "x"}` on the unit uses the same engine function, so it speaks too.

## How to give a unit a voice

1. Def (or an ext every def of the unit includes): `{props "voice_sm_dreadnought"}`
2. `set\talk.set`, inside `(define "phrases_tank" ...)`:
   `("phrase_voice_as" args %0 voice_sm_dreadnought 1 %1 sm_dreadnought)`
   (language = the whole prop, last word = the sound folder)
3. Sounds: `sound\talk\tank\sm_dreadnought\<event folder>\v1\*.wav`, same event folders as any tank nationality.

The unit also needs what every talking vehicle has: `{extender "talk" {talker "tank"}}` and `{able "talk"}`.
A unit with prop `tank` uses the `tank_*` talks, one with prop `cannon` the `cannon_*` talks.

## Settings (`gem_crewvoice.ini`)

| Key | Default | Meaning |
|---|---|---|
| Enabled | 1 | 0 = do nothing |
| Prefix | voice_ | start of the def prop that names the language |
| Server | 1 | also patch the dedicated server exe |
| Log | 0 | 1 = log the first 300 lines spoken this way |

Log file: `gem_crewvoice.log` (editor `gem_crewvoice_editor.log`, server `gem_crewvoice_server.log`) next to the
DLL. The first line says whether it installed.

## Multiplayer

It only changes which sound plays. Every player who wants to hear the lines needs the DLL.

## Build

`build.bat` (zig cc; set `ZIG_EXE` to a zig.exe or `pip install ziglang`). `build.bat test` runs the finders
and the engine's own Tag copy on the three 1.067 exes, then the fallback logic and the asm stub on fake objects.

Engine notes: `_notes\dll_hooks.md`, section "Engine talk on crewless units".
