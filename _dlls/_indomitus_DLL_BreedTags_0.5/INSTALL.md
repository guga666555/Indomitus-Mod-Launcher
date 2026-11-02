# GEM BreedTags 0.5 - install / uninstall

## Game side (every player, every process that simulates the battle)
Copy into `...\Call to Arms - Gates of Hell\binaries\x64\`:
- `dbghelp.dll`
- `gem_breedtags.ini`   (RequireMod= must match the mod.info name; it is a case-insensitive substring)

Start the game; `binaries\x64\gem_breedtags.log` says what it found. Alt+T in a battle = engine tag view.
Uninstall: delete the two files (and the .log).

## Mod side
- `mod_side/breed_tags.inc`  -> `resource\set\interaction_entity\40k_human\40k_skin\skin\breeds_traits\`
- `mod_side/skin_prop.inc`   -> replaces `...\40k_skin\skin_prop.inc` (diff in skin_prop.inc.diff: 3 lines)
- breeds: move the `*_tx_00` / `*_ID` inventory items into `{tags}` (see dk_commissar_bolt.set.example).
  Without the DLL the old item path still works, so this is safe to ship either way.
- When done testing, comment out the body of `Breed_Tags_Debug` in breed_tags.inc (bt_probe_* tags).

## Rebuild
`pip install ziglang` then `source/build.sh`. `gcc -DBT_TEST source/gem_breedtags.c -o bt_test &&
./bt_test call_to_arms.exe` checks a new game version offline.
