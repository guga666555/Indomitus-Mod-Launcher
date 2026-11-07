# GEM MeleeStance 0.6.10

Dawn of War style melee toggle + melee charge for Call to Arms: Gates of Hell 1.067 (game, editor, server).

**Stance:** while a human carries the script tag `melee_stance`, the engine's weapon/ammo choice (`set\target\ammo.set`)
only accepts melee ammo. The unit draws its melee weapon and keeps using it until the tag is removed.
Untagged units behave exactly like vanilla.

**Charge:** a human tagged `has_melee_weapon` with the melee weapon in hand (or in stance) who sees an enemy
beyond melee reach gets the engine's own attack order on that enemy (the double-click order), so it runs in.
Nearest seen enemy wins; settings in `[Charge]`. No mission triggers involved. One order per enemy: a new attack order
restarts the running one (the unit stops and re-paths), so nothing is re-issued while the unit is still closing on the enemy
it was sent at; only a stalled unit, a dead enemy or a much closer one gets a new order.
Beyond `CloseDistance` the charge is the player's double-click move (an `eMovementChange` mode order, then `Order::eMoveTask`;
the single-click move is `Order::eAdvance`, the slow weapon-up walk that forces its own mode 1); inside it the `attack_melee` order (`Order::eFight`, game exe+0x8E7550). A plain attack order on an enemy out of
weapon reach does not make a stance unit approach.

**Always-on / from triggers:** the tag `melee_always` (ini `AlwaysTag`) counts as stance but the button never
removes it. Put it in a breed's `{tags}` for permanent berserkers, or add/remove it from a mission trigger
(`{"entity_state" {selector â€¦} {tag_add melee_always}}`). A trigger can also add/remove `melee_stance` itself
(then the button can switch it back). Charge range = `[Charge] MaxDistance` (2000 units = 100 m); no charge
orders closer than `MinDistance` (60 units), where the unit is already fighting.

**Multi-select instant buttons:** vanilla runs an `{instant}` action button only when exactly one selected unit can
use it (`GetActor`, game exe+0xB73E90), so with a squad selected the button just sits pending. For actions with
`{apply_to_all}` the dll runs the vanilla `ExecuteInstant` (exe+0xB5E260, patched in the 9 action vtables) once per
eligible selected unit, each time as if only that unit were selected. `[MultiSelect] Enabled`.

**Melee through shields:** before a paired blow starts, the engine casts a ray attacker -> target and drops the
blow unless the first hit is the target (game exe+0x95E5C2). A forcefield is its own entity linked to the wearer,
so it was always hit first. `[ShieldMelee]` skips hits on entities linked to the target or tagged `forcefield`;
walls and other units still block.

**Blow reach:** `[BlowReach]` overwrites the engine's paired-melee start distances (vanilla `set\blow` fists/kick
= near 17, rifle butt/knife = medium 29, bayonet/sword = far 35 world units, centre to centre) with 45 / 45 / 45 (ini),
for every paired fight in the game. Table found by its reader `GetBlowDistance` (game exe+0x95EC10, table +0x10EDC80).

## Files
- `gem_meleestance.dll` - plain dll, hooks in DllMain (inject before the engine runs, or into a running game for tests)
- `gem_meleestance.ini` - settings (next to the dll)
- `gem_inject.exe` + `inject.bat` - in-house test: injects the dll into a running game/editor

## Settings
| key | default | meaning |
|---|---|---|
| Enabled | 1 | install the hook |
| Tag | melee_stance | script tag that turns the stance on |
| MeleeAmmoTags | melee_vs_human, melee_vs_all, medium_inf_melee_target, medium_veh_melee_target, heavy_veh_melee_target | ammo `{tag}`s that count as melee |
| SkipLinked | 1 | crews, gunners and passengers ignore the stance (a tagged man on a vehicle gun still fires it) |
| DrawOnToggle / DrawButton / DrawCooldownMs | 1 / melee_toggle / 3000 | toggling the stance on gives the unit the engine take-item order for the button's `{item}`; one order per unit per cooldown |
| DrawFromBelt | 1 | ammo.set melee rows marked `{ready_for_shot}` also allow drawing the weapon from the belt |
| Log | 1 | `gem_meleestance.log`: install line + first 40 stance decisions |

| [Charge] Enabled | 1 | melee units charge seen enemies |
| [Charge] Tag | has_melee_weapon | tag a unit needs to charge (stance units always qualify) |
| [Charge] MeleeInHand | 1 | only while the melee weapon is in hand |
| [Charge] PlayerUnits | 1 | 0 = AI-controlled units only |
| [Charge] CooldownMs / RetargetMs | 4000 / 1500 | order spacing per unit (same enemy / any enemy) |
| [Charge] CloseDistance / MoveRefreshUnits / RunMode | 200 / 80 / 2 | farther than CloseDistance: eMovementChange(RunMode) + eMoveTask (the double-click run) to the enemy (re-issued when it moved MoveRefreshUnits); inside it the attack_melee order (eFight) |
| [Charge] SwitchRatio / ProgressUnits | 0.6 / 10 | one order per enemy: no re-order while the unit is still closing on it; another enemy only when closer than SwitchRatio x the current one |
| AlwaysTag | melee_always | second stance tag for breeds / triggers, never removed by the button |
| [Charge] MaxDistance | 2000 | farthest enemy in world units (20 = 1 m), 0 = whatever the unit sees |
| [Charge] MinDistance | 60 | no new order closer than this (unit is already fighting) |

## Engine hook
`MatchAmmo(ctx)` (game exe+0x914180, editor +0x79AFB0, server +0x729370; found by pattern) is called once per
candidate ammo by every weapon choice path. The dll moves its first 14 bytes to a trampoline:
tagged + non-melee ammo -> no match; tagged + melee ammo -> original with the "in hand" flag set.
Charge: only calls from the engine's enemy scan (game exe+0x9580BA) count. A melee match against an enemy
beyond `Range(ammo)` queues `MakeAttack(targetRef)` (exe+0x8EA230) and `eActor::AddOrder` (exe+0x7EFA10) with the
UI's parameters; the order is issued on a later AI tick from another unit's scan, never inside the scan that found it.
Unit and target are held as engine target refs, so a deleted entity drops the order.

## Multiplayer
Weapon choice runs on every machine, so every player needs the dll (launcher-forced, like the other GEM dlls).

## Build
`build.bat` -> `dist\`, `build.bat payload` also copies dll + ini into `launcher\payload\` (then rebuild the launcher),
`build.bat test` = offline finders + filter/charge cases on the three 1.067 exes. Same zig flags as the other payload dlls.
The `gem_inject.exe` in-house injector is not part of the launcher build; the mod's `_tools\gem_meleestance\out\` has it.
