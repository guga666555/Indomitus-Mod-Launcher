# GEM ItemPierce 0.1: write-up

## Player-facing (patch notes)

**Space Marine helmets can now be penetrated.**
Helmets used to stop every round that hit them, so a headshot never hurt a helmeted Marine,
even from a lascannon. A round with enough penetration now goes through the helmet and hits the
head behind it, with its damage reduced by the helmet. Weak rounds such as lasguns still bounce off.
Take a helmet off and the head is unprotected; put it back on and the protection returns.

## For the launcher dev

**Problem.** Worn helmets are inventory items whose volume the mod re-enables (steel, 20 mm) so
they stop rounds. When a round pierced that volume, the engine's hit routine still returned "stop",
so the round died inside the helmet and never reached the head volume inside it.

**Fix.** `gem_itempen.dll` makes two mid-function patches in the bullet code. Both are found by
byte pattern at load, on game, editor and server 1.067.

1. Pierce result (HitVolume, game exe+0x83740F): after a pierce on an entity with the script tag
   `pierce_through`, the result becomes "carry on" instead of "stop". Rounds the engine marks as
   out of energy still stop.
2. Hit dedupe (BulletStep, game exe+0x835A9B): a tagged entity's hit record is never skipped and
   never becomes the dedupe key, so the wearer's volume behind it is still hit in the same step.

The mod adds the tag in script (`"stuff helmet_armor"` `{on spawn}` → `{tags add "pierce_through"}`),
so any other item can opt in the same way. Without the dll the tag does nothing.

**Integration.**
- Ship `bin\gem_itempen.dll` + `gem_itempen.ini` (same folder). Plain dll, hooks install in `DllMain`; inject before a map loads.
- Required on every machine in multiplayer (bullets are simulated in lockstep).
- Does not touch the SpawnBullet / Launch / Quant hooks used by weaponscript and bombbay.
- Logs to `gem_itempen.log` next to the dll; unknown exe build or already-patched sites → `NOT installed`, no patch.
- SHA256 dll `8ED5A2D4FBE307002DBD8CE646C53F2EB0D382CE6EAE5905EA4F7BB0DBDB8BCB`

**Status.** Tested working in-house (injected into the running game, Nov 4 2026). Multiplayer untested.
