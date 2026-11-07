# GEM ItemPierce 0.1

Rounds that **pierce** an armoured volume of a worn item keep flying into the wearer.
Built for Indomitus SM helmets. Call to Arms: Gates of Hell 1.067 (game, editor, server).

Status: tested working in-house (Nov 4 2026, injected at runtime). Multiplayer not tested yet.

## Why

A worn helmet's volume is re-enabled by script (`"stuff helmet_armor"`, 40k_equipment.inc) so its
20 mm steel stops rounds. When a round *does* get through, the engine's hit routine still answers
"stop", so the round dies inside the helmet. It never reaches the unarmoured `SM_head` volume
the helmet sphere encloses, so head hits did no damage at all.

## What the dll does

Two small patches in the bullet code, found by byte pattern at load time:

| Patch | Engine code (game exe) | Change |
|---|---|---|
| A, pierce result | HitVolume exe+0x836B40, site exe+0x83740F: `ebx = (flags & 2) ? 1 : 2` after a pierce | If the hit entity has the script tag, the result becomes 1 (round carries on). Skipped when the engine says the round is out of energy. |
| B, hit dedupe | BulletStep exe+0x835500, site exe+0x835A9B: skip a hit record whose `volume+0x38` key equals the previous one | A tagged entity's record is never skipped and does not become the key, so the wearer's volume right behind it is still hit in the same step. |

Editor sites: exe+0x6BAA72 / exe+0x6B909F. Server sites: exe+0x64C9FF / exe+0x64B08B.

The round keeps the damage scale the engine already cut for the pierce. The wearer then takes
the hit through its own script (`{on bullet_hit}` / `{on pierce}`), same as an unhelmeted head.

Which entities: any entity with the **script tag** `pierce_through` (ini `Tag=`), read from the
entity tag vector at entity+0x4E8 (`std::vector<Tag>`, 0x60 bytes each, name `std::string` at +0x40).
The mod adds it in `"stuff helmet_armor"` `{on spawn}`. Without the dll the tag does nothing.

## Files

- `gem_itempen.dll` + `gem_itempen.ini` (same folder; log `gem_itempen.log` / `_editor.log` / `_server.log` next to them)
- `gem_inject.exe`: in-house test helper only (loads the dll into a running game or editor)
- `gem_itempen.c`, `build.bat`, `gem_inject.c`: source (in `src\`; build.bat expects gem_inject.c at `..\gem_squadfollow\`, adjust the path if you rebuild from the zip) (zig cc: `pip install ziglang`; `build.bat test` = finder test on the three exes)

## In-house test (no launcher change)

1. Start the game or editor through the Indomitus launcher. Wait in the main menu.
2. Run `out\inject.bat` (= `gem_inject.exe gem_itempen.dll`).
3. `out\gem_itempen.log` (or `_editor.log`) should say `installed: pierce site exe+0x...`.
4. Shoot a helmeted marine in the head with something above 20 mm pen (e.g. a lascannon or AT rifle), and with
   something below (lasgun). The first should hurt or kill, the second should still bounce.
   With `Log=1` every pass-through writes a `pass:` line.

## Launcher integration

Plain dll (no proxy). Hooks are installed in `DllMain`, so it may be injected any time before a
map loads. Inject it like the other payload dlls; the ini sits next to it.

- Must run on **every** machine in multiplayer. Bullets are simulated in lockstep, so a client
  without it would disagree about who got hit.
- Patches are mid-function jumps (14-byte abs jmp + nops) at two sites no other GEM hook dll touches.
  SpawnBullet / Bullet::Launch / Bullet::Quant hooks (weaponscript, bombbay) are not affected.
- If a site is already patched or the exe build is unknown, it logs `NOT installed: ...` and does nothing.

## Package layout

- `bin\`: gem_itempen.dll, gem_itempen.ini (ship these)
- `tools\`: gem_inject.exe + inject.bat (test only, do not ship)
- `src\`: source

SHA256:
- gem_itempen.dll `8ED5A2D4FBE307002DBD8CE646C53F2EB0D382CE6EAE5905EA4F7BB0DBDB8BCB`
- gem_itempen.ini `08CC75BDAEC1A10A216ECE075787FCBBF6A3503FB88F44ECEF879CD1F6B491ED`
